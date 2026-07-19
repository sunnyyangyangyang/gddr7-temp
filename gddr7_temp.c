// gddr7_temp.c — minimal standalone kernel module (hwmon version, v6).
// Reads the RTX 5090 (GB202) GDDR7 DQR temperature sensors directly via
// ioremap and exposes them through the standard hwmon subsystem as
// 9 SEPARATE hwmon devices — one for hotspot, one per GDDR7 module —
// each with its own /sys/class/hwmon/hwmonN/{name,temp1_input,temp1_label}.
//
// WHY SEPARATE DEVICES INSTEAD OF ONE DEVICE WITH 9 CHANNELS:
// Several monitoring tools (lm-sensors' `sensors`, AstraMonitor, etc.)
// group/collapse all tempN_* channels under a single hwmon device as
// "one chip, N probes" and don't surface them as independently named
// sensors. To get 9 independently-named entries in those tools, each
// one needs to be its own hwmon chip instance (its own hwmonN dir with
// its own `name` file), not one chip with 9 temp channels.
//
// This board always has exactly 8 GDDR7 modules, fixed at compile time.
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
// hwmon_device_unregister() for every registered instance in exit().

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
#define DQR_NUM_SENSORS (DQR_NUM_MODULES + 1)  /* + hotspot */

/* Total span of BAR0 address space this module ever reads from,
 * starting at DQR_MODULE0. Used both to size the single ioremap and
 * to sanity-check BAR0 is actually big enough before we map it. */
#define DQR_SPAN ((DQR_NUM_MODULES - 1) * DQR_STRIDE + DQR_VLD_OFF + sizeof(u32))

static struct pci_dev *gpu_dev;
static void __iomem *dqr_base;   /* ioremap'd once, covers DQR_SPAN bytes */

/* Per-sensor identity. module_idx == -1 means "hotspot" (report the
 * max across all modules); module_idx >= 0 means "this one physical
 * module's own reading". chip_name becomes the hwmon `name` file
 * content, so it must be a short identifier with no whitespace —
 * that's what makes each one show up as a distinct, independently
 * named sensor instead of being folded into one chip.
 */
struct gddr7_sensor_ctx {
    int module_idx;
    const char *chip_name;
    const char *label;
};

static struct gddr7_sensor_ctx gddr7_sensors[DQR_NUM_SENSORS] = {
    { -1, "gddr7hotspot", "hotspot" },
    {  0, "gddr7mod0",    "module0" },
    {  1, "gddr7mod1",    "module1" },
    {  2, "gddr7mod2",    "module2" },
    {  3, "gddr7mod3",    "module3" },
    {  4, "gddr7mod4",    "module4" },
    {  5, "gddr7mod5",    "module5" },
    {  6, "gddr7mod6",    "module6" },
    {  7, "gddr7mod7",    "module7" },
};

/* One hwmon device handle per sensor above. Registered/unregistered
 * explicitly and symmetrically (see lifecycle note at top of file). */
static struct device *hwmon_devs[DQR_NUM_SENSORS];

/* Every sensor exposes exactly one temp channel (channel 0): its own
 * temp1_input / temp1_label. Shared across all 9 registrations since
 * the shape is identical; only drvdata differs per instance. */
static const u32 gddr7_temp_config[] = {
    HWMON_T_INPUT | HWMON_T_LABEL,
    0,
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
 * *valid_mask is set. Returns false if nothing came back valid at
 * all (including PM resume failure); true otherwise.
 *
 * Called once per sysfs read, even for a single-module sensor — it's
 * just a handful of MMIO reads, not worth the complexity of caching
 * across the 9 independent hwmon devices.
 */
static bool gddr7_read_modules(int temps[DQR_NUM_MODULES],
                                unsigned int *valid_mask, int *hottest)
{
    int hot = -128;
    unsigned int mask = 0;
    int pm_ret;
    int p;

    pm_ret = pm_runtime_resume_and_get(&gpu_dev->dev);
    if (pm_ret < 0)
        return false;

    for (p = 0; p < DQR_NUM_MODULES; p++) {
        int c;

        if (!gddr7_read_module(p, &c))
            continue;

        temps[p] = c;
        mask |= BIT(p);

        if (c > hot)
            hot = c;
    }

    pm_runtime_put(&gpu_dev->dev);

    *valid_mask = mask;
    *hottest = hot;
    return mask != 0;
}

/* --- hwmon callbacks (shared by all 9 device instances; drvdata tells
 * each call which sensor it is) --- */

static umode_t gddr7_hwmon_is_visible(const void *data,
                                      enum hwmon_sensor_types type,
                                      u32 attr, int channel)
{
    if (type != hwmon_temp || channel != 0)
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
    struct gddr7_sensor_ctx *ctx = dev_get_drvdata(dev);
    int temps[DQR_NUM_MODULES];
    unsigned int valid_mask;
    int hottest;

    if (type != hwmon_temp || attr != hwmon_temp_input || channel != 0)
        return -EOPNOTSUPP;

    if (!gddr7_read_modules(temps, &valid_mask, &hottest))
        return -ENODATA;

    if (ctx->module_idx < 0) {
        *val = hottest * 1000L;
        return 0;
    }

    if (!(valid_mask & BIT(ctx->module_idx)))
        return -ENODATA;

    *val = temps[ctx->module_idx] * 1000L;
    return 0;
}

static int gddr7_hwmon_read_string(struct device *dev,
                                    enum hwmon_sensor_types type,
                                    u32 attr, int channel, const char **str)
{
    struct gddr7_sensor_ctx *ctx = dev_get_drvdata(dev);

    if (type != hwmon_temp || attr != hwmon_temp_label || channel != 0)
        return -EOPNOTSUPP;

    *str = ctx->label;
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

static void gddr7_unregister_all(int upto)
{
    int i;

    for (i = upto - 1; i >= 0; i--) {
        if (hwmon_devs[i]) {
            hwmon_device_unregister(hwmon_devs[i]);
            hwmon_devs[i] = NULL;
        }
    }
}

static int __init gddr7_temp_init(void)
{
    resource_size_t bar0_len;
    int ret;
    int i;

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

    /* Register each sensor as its own hwmon chip instance so
     * monitoring tools show 9 independently-named sensors instead of
     * folding them into one chip with 9 temp channels. Deliberately
     * NOT devm_* — see lifecycle note at top of file. */
    for (i = 0; i < DQR_NUM_SENSORS; i++) {
        hwmon_devs[i] = hwmon_device_register_with_info(&gpu_dev->dev,
                                gddr7_sensors[i].chip_name,
                                &gddr7_sensors[i],
                                &gddr7_chip_info, NULL);
        if (IS_ERR(hwmon_devs[i])) {
            ret = PTR_ERR(hwmon_devs[i]);
            hwmon_devs[i] = NULL;
            pr_err("gddr7_temp: hwmon registration failed for %s: %d\n",
                   gddr7_sensors[i].chip_name, ret);
            goto err_unregister;
        }
    }

    pr_info("gddr7_temp: found RTX 5090 at %s, registered %d hwmon devices\n",
             pci_name(gpu_dev), DQR_NUM_SENSORS);

    return 0;

err_unregister:
    gddr7_unregister_all(i);
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
    /* Explicit, symmetric teardown for every registered instance —
     * must happen before iounmap/pci teardown so no read callback can
     * race with the resources being freed underneath it. */
    gddr7_unregister_all(DQR_NUM_SENSORS);

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
MODULE_DESCRIPTION("Read RTX 5090 GDDR7 DQR temperature sensors via hwmon (9 independent devices)");