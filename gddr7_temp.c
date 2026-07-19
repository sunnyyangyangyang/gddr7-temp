// gddr7_temp.c — minimal standalone kernel module (hwmon version, v5).
// Reads the RTX 5090 (GB202) GDDR7 DQR temperature sensors directly via
// ioremap and exposes them through the standard hwmon subsystem
// (/sys/class/hwmon/hwmonX/tempN_input).
//
// This board always has exactly 8 GDDR7 modules, so the hwmon channel
// layout is fixed at compile time: channel 0 = hotspot, channels 1..8
// = module0..module7.
//
// NOTE: reverse-engineered, unofficial. No PLM bypass — this reads a
// register that simply isn't privilege-locked, it doesn't defeat any
// hardware protection. Read-only, no writes to GPU MMIO anywhere.
//
// IMPORTANT lifecycle note (read before touching this file):
// We deliberately do NOT use devm_hwmon_device_register_with_info().
// gpu_dev is a device we merely looked up via pci_get_device() — it is
// bound to nvidia.ko (or nouveau), not to this module. devm_* resources
// attached to &gpu_dev->dev are only released when THAT device unbinds
// from ITS driver, which never happens just because we rmmod this
// module. Using the devm_ variant here would leave the hwmon sysfs
// files (and their function pointers, pointing into this module's
// .text) alive in memory after rmmod, and the next read of e.g.
// tempN_input would jump into freed module memory -> oops/panic.
// So: explicit hwmon_device_register_with_info() in init(), explicit
// hwmon_device_unregister() in exit() — same pairing discipline as the
// old proc_create()/proc_remove() version.

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/io.h>
#include <linux/pm_runtime.h>
#include <linux/hwmon.h>
#include <linux/bitops.h>
#include <linux/err.h>

#define NV_VENDOR_ID   0x10de
#define RTX5090_DEV_ID 0x2b85

#define DQR_MODULE0     0x009024C0u
#define DQR_VLD_OFF     0x10u          /* DQR_MODULE0 + 0x10 = validity reg */
#define DQR_STRIDE      0x00004000u
#define DQR_NUM_MODULES 8              /* fixed: this board always has 8 */

/* Total span of BAR0 address space this module ever reads from,
 * starting at DQR_MODULE0. Used both to size the single ioremap and
 * to sanity-check BAR0 is actually big enough before we map it. */
#define DQR_SPAN ((DQR_NUM_MODULES - 1) * DQR_STRIDE + DQR_VLD_OFF + sizeof(u32))

static struct pci_dev *gpu_dev;
static void __iomem *dqr_base;   /* ioremap'd once, covers DQR_SPAN bytes */
static struct device *hwmon_dev; /* explicitly registered/unregistered, no devm */

/* channel 0 = hotspot, channels 1..DQR_NUM_MODULES = each physical
 * module, final 0 = terminator. Fixed size, known at compile time. */
static const u32 gddr7_temp_config[DQR_NUM_MODULES + 2] = {
    [0 ... DQR_NUM_MODULES] = HWMON_T_INPUT | HWMON_T_LABEL,
    [DQR_NUM_MODULES + 1] = 0,
};

static const struct hwmon_channel_info gddr7_temp_info = {
    .type = hwmon_temp,
    .config = gddr7_temp_config,
};

static const struct hwmon_channel_info *gddr7_info[] = {
    &gddr7_temp_info,
    NULL
};

/* Convert raw GDDR temp MR-code (bits 23:16 of the DQR data word) to °C. */
static int decode_mrcode(u32 raw)
{
    int code = (raw >> 16) & 0xFF;
    if (code > 80)
        code = 80;
    return (code > 19) ? (code - 20) * 2 : -(40 - code * 2);
}

/*
 * Read one module's DQR validity + data words from the pre-mapped
 * region. Returns true and fills *out_c with the decoded Celsius value
 * if the module reports a genuinely valid, non-poisoned reading;
 * returns false otherwise (including on a raw 0xFFFFFFFF bus-error
 * readback, which we never trust as real data).
 */
static bool gddr7_read_module(int module_idx, int *out_c)
{
    u32 off = (u32)module_idx * DQR_STRIDE;
    u32 vld = ioread32(dqr_base + off + DQR_VLD_OFF);
    u32 dq  = ioread32(dqr_base + off);

    /* A stuck-at-0xFFFFFFFF readback means the bus transaction failed
     * (device asleep, removed, or otherwise unreachable) — never
     * decode it as sensor data. */
    if (vld == 0xFFFFFFFFu || dq == 0xFFFFFFFFu)
        return false;

    if (((vld >> 24) & 0xF) != 0xF)          /* not all 4 IC/subp valid */
        return false;
    if ((dq & 0xFFFF0000u) == 0xBADF0000u)   /* poison sentinel */
        return false;

    *out_c = decode_mrcode(dq);
    return true;
}

/* Wake the GPU (best-effort), read all 8 modules into temps[0..7],
 * and report the hottest reading. temps[i] is only valid if bit i of
 * *valid_mask is set. Returns false if the GPU couldn't be woken /
 * nothing came back valid at all; true otherwise (even if only some
 * modules came back valid).
 */
static bool gddr7_read_modules(int temps[DQR_NUM_MODULES],
                                unsigned int *valid_mask, int *hottest)
{
    int hot = -128;
    unsigned int mask = 0;
    int pm_ret;
    int p;

    pm_ret = pm_runtime_resume_and_get(&gpu_dev->dev);
    if (pm_ret < 0) {
        /* Runtime PM disabled, device gone, or resume failed.
         * Nothing was acquired, nothing to unwind. */
        return false;
    }

    for (p = 0; p < DQR_NUM_MODULES; p++) {
        int c;

        if (!gddr7_read_module(p, &c))
            continue;

        temps[p] = c;
        mask |= BIT(p);

        if (c > hot)
            hot = c;
    }

    /* Drop the runtime PM reference acquired above. We intentionally
     * do not alter autosuspend policies or timings so as not to
     * interfere with the NVIDIA driver's own PM state machine. */
    pm_runtime_put(&gpu_dev->dev);

    *valid_mask = mask;
    *hottest = hot;
    return mask != 0;
}

/* --- hwmon callbacks --- */

static umode_t gddr7_hwmon_is_visible(const void *data,
                                      enum hwmon_sensor_types type,
                                      u32 attr, int channel)
{
    if (type != hwmon_temp || channel > DQR_NUM_MODULES)
        return 0;

    switch (attr) {
    case hwmon_temp_input:
    case hwmon_temp_label:
        return 0444;
    default:
        return 0;
    }
}

static int gddr7_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
                             u32 attr, int channel, long *val)
{
    int temps[DQR_NUM_MODULES];
    unsigned int valid_mask;
    int hottest;

    if (type != hwmon_temp || attr != hwmon_temp_input)
        return -EOPNOTSUPP;

    if (!gddr7_read_modules(temps, &valid_mask, &hottest))
        return -ENODATA;

    if (channel == 0) {
        *val = hottest * 1000L;
        return 0;
    }

    if (!(valid_mask & BIT(channel - 1)))
        return -ENODATA;

    *val = temps[channel - 1] * 1000L;
    return 0;
}

static int gddr7_hwmon_read_string(struct device *dev,
                                    enum hwmon_sensor_types type,
                                    u32 attr, int channel, const char **str)
{
    static const char * const labels[DQR_NUM_MODULES] = {
        "module0", "module1", "module2", "module3",
        "module4", "module5", "module6", "module7",
    };

    if (type != hwmon_temp || attr != hwmon_temp_label)
        return -EOPNOTSUPP;

    *str = (channel == 0) ? "hotspot" : labels[channel - 1];
    return 0;
}

static const struct hwmon_ops gddr7_hwmon_ops = {
    .is_visible  = gddr7_hwmon_is_visible,
    .read        = gddr7_hwmon_read,
    .read_string = gddr7_hwmon_read_string,
};

static const struct hwmon_chip_info gddr7_chip_info = {
    .ops  = &gddr7_hwmon_ops,
    .info = gddr7_info,
};

static int __init gddr7_temp_init(void)
{
    resource_size_t bar0_len;
    int ret;

    gpu_dev = pci_get_device(NV_VENDOR_ID, RTX5090_DEV_ID, NULL);
    if (!gpu_dev) {
        pr_warn("gddr7_temp: no RTX 5090 (dev id 0x%04x) found\n", RTX5090_DEV_ID);
        return -ENODEV;
    }

    bar0_len = pci_resource_len(gpu_dev, 0);
    if (bar0_len < DQR_MODULE0 + DQR_SPAN) {
        pr_err("gddr7_temp: BAR0 too small (0x%llx bytes), refusing to map\n",
               (unsigned long long)bar0_len);
        ret = -EINVAL;
        goto err_put;
    }

    /* Make sure MMIO decode is actually enabled on this device before
     * we try to read it. Refcounted, so this is safe even if another
     * driver (e.g. nvidia.ko) already has the device enabled. */
    ret = pci_enable_device_mem(gpu_dev);
    if (ret) {
        pr_err("gddr7_temp: pci_enable_device_mem failed: %d\n", ret);
        goto err_put;
    }

    dqr_base = ioremap(pci_resource_start(gpu_dev, 0) + DQR_MODULE0, DQR_SPAN);
    if (!dqr_base) {
        pr_err("gddr7_temp: ioremap failed\n");
        ret = -ENOMEM;
        goto err_disable;
    }

    /* Deliberately NOT devm_hwmon_device_register_with_info() — see
     * the big comment at the top of this file. gpu_dev belongs to
     * nvidia.ko's driver binding, not ours, so a devm_ resource hung
     * off &gpu_dev->dev would never get released on rmmod of THIS
     * module. We register/unregister explicitly instead, paired with
     * hwmon_device_unregister() in gddr7_temp_exit(). */
    hwmon_dev = hwmon_device_register_with_info(&gpu_dev->dev,
                            "gddr7temp", NULL, &gddr7_chip_info, NULL);
    if (IS_ERR(hwmon_dev)) {
        ret = PTR_ERR(hwmon_dev);
        hwmon_dev = NULL;
        pr_err("gddr7_temp: hwmon registration failed: %d\n", ret);
        goto err_unmap;
    }

    pr_info("gddr7_temp: found RTX 5090 at %s, registered hwmon (%d modules + hotspot)\n",
             pci_name(gpu_dev), DQR_NUM_MODULES);

    return 0;

err_unmap:
    iounmap(dqr_base);
    dqr_base = NULL;
err_disable:
    pci_disable_device(gpu_dev);
err_put:
    pci_dev_put(gpu_dev);
    gpu_dev = NULL;
    return ret;
}

static void __exit gddr7_temp_exit(void)
{
    /* Explicit, symmetric teardown — mirrors the old proc_remove()
     * discipline. Must happen before we free dqr_base / touch gpu_dev,
     * so that no hwmon read callback can race with the unmap below. */
    if (hwmon_dev) {
        hwmon_device_unregister(hwmon_dev);
        hwmon_dev = NULL;
    }

    if (dqr_base) {
        iounmap(dqr_base);
        dqr_base = NULL;
    }

    if (gpu_dev) {
        pci_disable_device(gpu_dev);
        pci_dev_put(gpu_dev);
        gpu_dev = NULL;
    }
}

module_init(gddr7_temp_init);
module_exit(gddr7_temp_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("you");
MODULE_DESCRIPTION("Read RTX 5090 GDDR7 DQR temperature sensors via hwmon");