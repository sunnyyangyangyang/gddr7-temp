// gddr7_temp.c — minimal standalone kernel module (hwmon version, v7).
//
// Reads the RTX 5090 (GB202):
//   (A) GDDR7 DQR temperature sensors  — as before (8 modules + max hotspot)
//   (B) THERM module internal hotspot channels — NEW in this version:
//       6 raw internal temperature channels at fixed BAR0 offsets
//       0xAD0A90 .. 0xAD0AA4 (32-bit registers, 4 bytes apart), plus a
//       derived 7th sensor reporting the max of those 6.
//
// All of it is exposed through the standard hwmon subsystem as
// independent hwmon devices — one per physical/derived sensor — so
// tools like `sensors` show each one under its own name instead of
// folding everything into one chip with N temp channels.
//
// THERM decode (per the reverse-engineered/igor'sLAB-corroborated model):
//   raw = ioread32(BAR0 + 0xAD0A90 + 4*channel)
//   valid  = bit 30 of raw
//   temp_C = (raw & 0xFFFF) / 256.0
// This is NOT an NVIDIA-documented interface. Treat values as
// experimental/best-effort, same caveat as upstream research notes.
//
// NOTE: reverse-engineered, unofficial. No PLM bypass — these are
// plain MMIO reads of registers that aren't privilege-locked; nothing
// here defeats a hardware protection. Read-only, no writes to GPU
// MMIO anywhere.
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

/* ---------------- GDDR7 DQR region (unchanged from v6) --------------- */

#define DQR_MODULE0     0x009024C0u
#define DQR_VLD_OFF     0x10u          /* DQR_MODULE0 + 0x10 = validity reg */
#define DQR_STRIDE      0x00004000u
#define DQR_NUM_MODULES 8              /* fixed: this board always has 8 */

/* Total span of BAR0 address space this module reads from for DQR,
 * starting at DQR_MODULE0. Used both to size the ioremap and to
 * sanity-check BAR0 is actually big enough before we map it. */
#define DQR_SPAN ((DQR_NUM_MODULES - 1) * DQR_STRIDE + DQR_VLD_OFF + sizeof(u32))

/* ---------------- THERM internal hotspot region (NEW) ---------------- */
//
// Six consecutive 32-bit registers inside Blackwell's NV_THERM module
// window (same window as the documented NV_THERM_I2CS_SCRATCH at
// 0xAD00BC in the nouveau/NVIDIA-copyrighted GB202 headers).

#define THERM_CH0        0x00AD0A90u
#define THERM_CH_STRIDE   0x00000004u
#define THERM_NUM_CHANNELS 6

#define THERM_VALID_BIT   BIT(30)
#define THERM_RAW_MASK    0xFFFFu      /* fixed point, 1/256 °C per LSB */

#define THERM_SPAN ((THERM_NUM_CHANNELS - 1) * THERM_CH_STRIDE + sizeof(u32))

static struct pci_dev *gpu_dev;
static void __iomem *dqr_base;    /* ioremap'd once, covers DQR_SPAN bytes   */
static void __iomem *therm_base;  /* ioremap'd once, covers THERM_SPAN bytes */

/* ---------------- sensor identity / dispatch ---------------- */

enum sensor_family {
    FAM_GDDR7,  /* idx == -1: max-of-8 hotspot; idx >= 0: single module   */
    FAM_THERM,  /* idx == -1: max-of-6 hotspot; idx >= 0: single channel  */
};

struct gpu_sensor_ctx {
    enum sensor_family family;
    int idx;
    const char *chip_name;  /* becomes the hwmon `name` file -> must be a
                              * short identifier, no whitespace, so each
                              * shows up as its own distinct sensor */
    const char *label;
};

static struct gpu_sensor_ctx gpu_sensors[] = {
    /* --- GDDR7 DQR (8 modules + hotspot) --- */
    { FAM_GDDR7, -1, "gddr7hotspot", "hotspot" },
    { FAM_GDDR7,  0, "gddr7mod0",    "module0" },
    { FAM_GDDR7,  1, "gddr7mod1",    "module1" },
    { FAM_GDDR7,  2, "gddr7mod2",    "module2" },
    { FAM_GDDR7,  3, "gddr7mod3",    "module3" },
    { FAM_GDDR7,  4, "gddr7mod4",    "module4" },
    { FAM_GDDR7,  5, "gddr7mod5",    "module5" },
    { FAM_GDDR7,  6, "gddr7mod6",    "module6" },
    { FAM_GDDR7,  7, "gddr7mod7",    "module7" },

    /* --- THERM internal channels (6 raw + 1 derived max) --- */
    { FAM_THERM,  0, "thermch0",      "therm_ch0" },
    { FAM_THERM,  1, "thermch1",      "therm_ch1" },
    { FAM_THERM,  2, "thermch2",      "therm_ch2" },
    { FAM_THERM,  3, "thermch3",      "therm_ch3" },
    { FAM_THERM,  4, "thermch4",      "therm_ch4" },
    { FAM_THERM,  5, "thermch5",      "therm_ch5" },
    { FAM_THERM, -1, "thermhotspot",  "hotspot_max" },
};

#define NUM_TOTAL_SENSORS ARRAY_SIZE(gpu_sensors)

/* One hwmon device handle per sensor above. Registered/unregistered
 * explicitly and symmetrically (see lifecycle note at top of file). */
static struct device *hwmon_devs[ARRAY_SIZE(gpu_sensors)];

/* Every sensor exposes exactly one temp channel (channel 0): its own
 * temp1_input / temp1_label. Shared across all registrations since the
 * shape is identical; only drvdata differs per instance. */
static const u32 gpu_temp_config[] = {
    HWMON_T_INPUT | HWMON_T_LABEL,
    0,
};

static const struct hwmon_channel_info gpu_temp_info = {
    .type = hwmon_temp,
    .config = gpu_temp_config,
};

static const struct hwmon_channel_info *gpu_info[] = {
    &gpu_temp_info,
    NULL
};

/* ---------------- GDDR7 DQR read path (unchanged logic) ---------------- */

/* Convert raw GDDR temp MR-code (bits 23:16 of the DQR data word) to °C. */
static int decode_mrcode(u32 raw)
{
    int code = (raw >> 16) & 0xFF;
    if (code > 80)
        code = 80;
    return (code > 19) ? (code - 20) * 2 : -(40 - code * 2);
}

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

/* Read all 8 GDDR7 modules into temps[0..7] and report the hottest. */
static bool gddr7_read_modules(int temps[DQR_NUM_MODULES],
                                unsigned int *valid_mask, int *hottest)
{
    int hot = -128;
    unsigned int mask = 0;
    int p;

    for (p = 0; p < DQR_NUM_MODULES; p++) {
        int c;

        if (!gddr7_read_module(p, &c))
            continue;

        temps[p] = c;
        mask |= BIT(p);

        if (c > hot)
            hot = c;
    }

    *valid_mask = mask;
    *hottest = hot;
    return mask != 0;
}

/* ---------------- THERM internal hotspot read path (NEW) ---------------- */

/* Read one THERM channel. Returns true and fills *out_mC (millidegree C)
 * if bit 30 marks it valid and the readback isn't a bus-error pattern.
 * *out_mC is computed as (raw & 0xFFFF) * 1000 / 256 to keep everything
 * in integer millidegree units for hwmon, equivalent to (raw & 0xFFFF)/256
 * degrees Celsius. */
static bool therm_read_channel(int ch, int *out_mC)
{
    u32 raw = ioread32(therm_base + (u32)ch * THERM_CH_STRIDE);

    if (raw == 0xFFFFFFFFu)
        return false;
    if (!(raw & THERM_VALID_BIT))
        return false;

    *out_mC = (int)(((raw & THERM_RAW_MASK) * 1000u) / 256u);
    return true;
}

/* Read all 6 THERM channels into temps_mC[0..5] (millidegree C) and
 * report the hottest valid one. */
static bool therm_read_channels(int temps_mC[THERM_NUM_CHANNELS],
                                 unsigned int *valid_mask, int *hottest_mC)
{
    int hot = INT_MIN;
    unsigned int mask = 0;
    int ch;

    for (ch = 0; ch < THERM_NUM_CHANNELS; ch++) {
        int mC;

        if (!therm_read_channel(ch, &mC))
            continue;

        temps_mC[ch] = mC;
        mask |= BIT(ch);

        if (mC > hot)
            hot = mC;
    }

    *valid_mask = mask;
    *hottest_mC = hot;
    return mask != 0;
}

/* ---------------- shared hwmon callbacks ---------------- */

static umode_t gpu_hwmon_is_visible(const void *data,
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

static int gpu_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
                           u32 attr, int channel, long *val)
{
    struct gpu_sensor_ctx *ctx = dev_get_drvdata(dev);
    int pm_ret;
    int ret = 0;

    if (type != hwmon_temp || attr != hwmon_temp_input || channel != 0)
        return -EOPNOTSUPP;

    pm_ret = pm_runtime_resume_and_get(&gpu_dev->dev);
    if (pm_ret < 0)
        return pm_ret;

    if (ctx->family == FAM_GDDR7) {
        int temps[DQR_NUM_MODULES];
        unsigned int valid_mask;
        int hottest;

        if (!gddr7_read_modules(temps, &valid_mask, &hottest)) {
            ret = -ENODATA;
            goto out;
        }

        if (ctx->idx < 0) {
            *val = hottest * 1000L;
        } else if (valid_mask & BIT(ctx->idx)) {
            *val = temps[ctx->idx] * 1000L;
        } else {
            ret = -ENODATA;
        }
    } else { /* FAM_THERM */
        int temps_mC[THERM_NUM_CHANNELS];
        unsigned int valid_mask;
        int hottest_mC;

        if (!therm_read_channels(temps_mC, &valid_mask, &hottest_mC)) {
            ret = -ENODATA;
            goto out;
        }

        if (ctx->idx < 0) {
            *val = hottest_mC;
        } else if (valid_mask & BIT(ctx->idx)) {
            *val = temps_mC[ctx->idx];
        } else {
            ret = -ENODATA;
        }
    }

out:
    pm_runtime_put(&gpu_dev->dev);
    return ret;
}

static int gpu_hwmon_read_string(struct device *dev,
                                  enum hwmon_sensor_types type,
                                  u32 attr, int channel, const char **str)
{
    struct gpu_sensor_ctx *ctx = dev_get_drvdata(dev);

    if (type != hwmon_temp || attr != hwmon_temp_label || channel != 0)
        return -EOPNOTSUPP;

    *str = ctx->label;
    return 0;
}

static const struct hwmon_ops gpu_hwmon_ops = {
    .is_visible  = gpu_hwmon_is_visible,
    .read        = gpu_hwmon_read,
    .read_string = gpu_hwmon_read_string,
};

static const struct hwmon_chip_info gpu_chip_info = {
    .ops  = &gpu_hwmon_ops,
    .info = gpu_info,
};

/* ---------------- registration / teardown ---------------- */

static void gpu_unregister_all(int upto)
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
    resource_size_t needed;
    int ret;
    int i;

    gpu_dev = pci_get_device(NV_VENDOR_ID, RTX5090_DEV_ID, NULL);
    if (!gpu_dev) {
        pr_warn("gddr7_temp: no RTX 5090 (dev id 0x%04x) found\n", RTX5090_DEV_ID);
        return -ENODEV;
    }

    bar0_len = pci_resource_len(gpu_dev, 0);

    needed = DQR_MODULE0 + DQR_SPAN;
    if (THERM_CH0 + THERM_SPAN > needed)
        needed = THERM_CH0 + THERM_SPAN;

    if (bar0_len < needed) {
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
        pr_err("gddr7_temp: ioremap of DQR region failed\n");
        ret = -ENOMEM;
        goto err_disable;
    }

    therm_base = ioremap(pci_resource_start(gpu_dev, 0) + THERM_CH0, THERM_SPAN);
    if (!therm_base) {
        pr_err("gddr7_temp: ioremap of THERM region failed\n");
        ret = -ENOMEM;
        goto err_unmap_dqr;
    }

    /* Register each sensor as its own hwmon chip instance so
     * monitoring tools show independently-named sensors instead of
     * folding them into one chip with N temp channels. Deliberately
     * NOT devm_* — see lifecycle note at top of file. */
    for (i = 0; i < NUM_TOTAL_SENSORS; i++) {
        hwmon_devs[i] = hwmon_device_register_with_info(&gpu_dev->dev,
                                gpu_sensors[i].chip_name,
                                &gpu_sensors[i],
                                &gpu_chip_info, NULL);
        if (IS_ERR(hwmon_devs[i])) {
            ret = PTR_ERR(hwmon_devs[i]);
            hwmon_devs[i] = NULL;
            pr_err("gddr7_temp: hwmon registration failed for %s: %d\n",
                   gpu_sensors[i].chip_name, ret);
            goto err_unregister;
        }
    }

    pr_info("gddr7_temp: found RTX 5090 at %s, registered %zu hwmon devices "
             "(8 GDDR7 modules + 1 hotspot, 6 THERM channels + 1 hotspot)\n",
             pci_name(gpu_dev), NUM_TOTAL_SENSORS);

    return 0;

err_unregister:
    gpu_unregister_all(i);
    iounmap(therm_base);
    therm_base = NULL;
err_unmap_dqr:
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
    gpu_unregister_all(NUM_TOTAL_SENSORS);

    if (therm_base) {
        iounmap(therm_base);
        therm_base = NULL;
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
MODULE_AUTHOR("Sunny Yang <yxh9956@gmail.com>");
MODULE_DESCRIPTION("Read RTX 5090 GDDR7 DQR + THERM internal hotspot sensors via hwmon");