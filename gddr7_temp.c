// gddr7_temp.c — minimal standalone kernel module (hwmon version, v8).
//
// Reads supported NVIDIA GPUs (offset tables in offsets.yaml):
//   (A) GDDR7 DQR temperature sensors — per-model module count + max hotspot
//   (B) THERM module internal hotspot channels:
//       per-model channel count at fixed BAR0 offsets, plus a derived sensor
//       reporting the max of all valid channels.
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

#include "gpu_offsets.h"
#include "gpu_offsets_generated.h"

#define NV_VENDOR_ID   0x10de

/* THERM protocol constants (not GPU-specific — same for all models) */
#define THERM_VALID_BIT   BIT(30)
#define THERM_RAW_MASK    0xFFFFu      /* fixed point, 1/256 °C per LSB */

/* Reasonable upper bounds for stack arrays in read callbacks.
 * Enforced at init() via sanity checks on active_table fields. */
#define GPU_MAX_DQR_MODULES   16
#define GPU_MAX_THERM_CHS     16

static struct pci_dev *gpu_dev;
static const struct gpu_offset_table *active_table;  /* set during init() */
static void __iomem *dqr_base;    /* ioremap'd once, covers computed dqr_span   */
static void __iomem *therm_base;  /* ioremap'd once, covers computed span   */

/* Dynamic sensor arrays (sized at init based on active_table) */
static struct gpu_sensor_ctx *gpu_sensors;
static int num_total_sensors;

/* ---------------- lookup / helpers ---------------- */

static const struct gpu_offset_table *find_table(u16 device_id)
{
    int i;
    for (i = 0; i < gpu_tables_count; i++)
        if (gpu_tables[i].device_id == device_id)
            return &gpu_tables[i];
    return NULL;
}

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

/* gpu_sensors[] is dynamically allocated in init() based on active_table.
 * hwmon_devs[] mirrors it one-to-one. Both are freed in exit(). */
static struct device **hwmon_devs;

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
    u32 off = (u32)module_idx * active_table->dqr_stride;
    u32 vld = ioread32(dqr_base + off + active_table->dqr_vld_off);
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

/* Read all GDDR7 modules into temps[] and report the hottest. */
static bool gddr7_read_modules(int *temps, unsigned int *valid_mask, int *hottest)
{
    int hot = -128;
    unsigned int mask = 0;
    int p;

    for (p = 0; p < active_table->dqr_num_modules; p++) {
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
    u32 raw = ioread32(therm_base + (u32)ch * active_table->therm_ch_stride);

    if (raw == 0xFFFFFFFFu)
        return false;
    if (!(raw & THERM_VALID_BIT))
        return false;

    *out_mC = (int)(((raw & THERM_RAW_MASK) * 1000u) / 256u);
    return true;
}

/* Read all THERM channels into temps_mC[] (millidegree C) and report the hottest. */
static bool therm_read_channels(int *temps_mC, unsigned int *valid_mask, int *hottest_mC)
{
    int hot = INT_MIN;
    unsigned int mask = 0;
    int ch;

    for (ch = 0; ch < active_table->therm_num_channels; ch++) {
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
        int temps[GPU_MAX_DQR_MODULES];
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
        int temps_mC[GPU_MAX_THERM_CHS];
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
    struct pci_dev *cand = NULL;
    resource_size_t bar0_len;
    resource_size_t dqr_span, therm_span;
    resource_size_t needed;
    int n_dqr_sensors, n_therm_sensors;
    int ret;
    int i;

    /* Iterate all NVIDIA devices until we find one in our offset table. */
    gpu_dev = NULL;
    while ((cand = pci_get_device(NV_VENDOR_ID, PCI_ANY_ID, cand))) {
        active_table = find_table(cand->device);
        if (active_table) {
            gpu_dev = cand;  /* hold this reference; loop won't put it */
            break;
        }
    }

    if (!gpu_dev) {
        pr_warn("gddr7_temp: no supported NVIDIA GPU found\n");
        return -ENODEV;
    }

    /* Sanity-check table fields to catch YAML typos early. */
    if (active_table->dqr_num_modules <= 0 || active_table->dqr_num_modules > GPU_MAX_DQR_MODULES) {
        pr_err("gddr7_temp: invalid dqr_num_modules %d for %s\n",
               active_table->dqr_num_modules, active_table->name);
        ret = -EINVAL;
        goto err_put;
    }
    if (active_table->therm_num_channels <= 0 || active_table->therm_num_channels > GPU_MAX_THERM_CHS) {
        pr_err("gddr7_temp: invalid therm_num_channels %d for %s\n",
               active_table->therm_num_channels, active_table->name);
        ret = -EINVAL;
        goto err_put;
    }

    /* Compute runtime spans from table. */
    dqr_span = (resource_size_t)(active_table->dqr_num_modules - 1) * active_table->dqr_stride
               + active_table->dqr_vld_off + sizeof(u32);
    therm_span = (resource_size_t)(active_table->therm_num_channels - 1) * active_table->therm_ch_stride
                 + sizeof(u32);

    bar0_len = pci_resource_len(gpu_dev, 0);

    needed = active_table->dqr_module0 + dqr_span;
    if (active_table->therm_ch0 + therm_span > needed)
        needed = active_table->therm_ch0 + therm_span;

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

    dqr_base = ioremap(pci_resource_start(gpu_dev, 0) + active_table->dqr_module0, dqr_span);
    if (!dqr_base) {
        pr_err("gddr7_temp: ioremap of DQR region failed\n");
        ret = -ENOMEM;
        goto err_disable;
    }

    therm_base = ioremap(pci_resource_start(gpu_dev, 0) + active_table->therm_ch0, therm_span);
    if (!therm_base) {
        pr_err("gddr7_temp: ioremap of THERM region failed\n");
        ret = -ENOMEM;
        goto err_unmap_dqr;
    }

    /* Build dynamic sensor array. Layout:
     * [DQR hotspot][DQR modules...][THERM channels...][THERM hotspot] */
    n_dqr_sensors  = active_table->dqr_num_modules + 1;   /* +1 for DQR hotspot */
    n_therm_sensors = active_table->therm_num_channels + 1; /* +1 for THERM hotspot */
    num_total_sensors = n_dqr_sensors + n_therm_sensors;

    gpu_sensors = kcalloc(num_total_sensors, sizeof(*gpu_sensors), GFP_KERNEL);
    if (!gpu_sensors) {
        ret = -ENOMEM;
        goto err_unmap_therm;
    }

    hwmon_devs = kcalloc(num_total_sensors, sizeof(*hwmon_devs), GFP_KERNEL);
    if (!hwmon_devs) {
        ret = -ENOMEM;
        goto err_free_sensors;
    }

    /* DQR hotspot (index 0) */
    gpu_sensors[0].family   = FAM_GDDR7;
    gpu_sensors[0].idx      = -1;
    gpu_sensors[0].chip_name = kasprintf(GFP_KERNEL, "gddr7hotspot");
    gpu_sensors[0].label     = "hotspot";

    /* DQR modules (indices 1..n) */
    for (i = 0; i < active_table->dqr_num_modules; i++) {
        int pos = i + 1;
        gpu_sensors[pos].family   = FAM_GDDR7;
        gpu_sensors[pos].idx      = i;
        gpu_sensors[pos].chip_name = kasprintf(GFP_KERNEL, "gddr7mod%d", i);
        gpu_sensors[pos].label     = gpu_sensors[pos].chip_name;  /* same string */
    }

    /* THERM channels (indices n_dqr..n) */
    for (i = 0; i < active_table->therm_num_channels; i++) {
        int pos = n_dqr_sensors + i;
        gpu_sensors[pos].family   = FAM_THERM;
        gpu_sensors[pos].idx      = i;
        gpu_sensors[pos].chip_name = kasprintf(GFP_KERNEL, "thermch%d", i);
        gpu_sensors[pos].label     = gpu_sensors[pos].chip_name;  /* same string */
    }

    /* THERM hotspot (last index) */
    {
        int pos = num_total_sensors - 1;
        gpu_sensors[pos].family   = FAM_THERM;
        gpu_sensors[pos].idx      = -1;
        gpu_sensors[pos].chip_name = kasprintf(GFP_KERNEL, "thermhotspot");
        gpu_sensors[pos].label     = "hotspot_max";
    }

    /* Check for kasprintf failures */
    for (i = 0; i < num_total_sensors; i++) {
        if (!gpu_sensors[i].chip_name) {
            ret = -ENOMEM;
            goto err_free_names;
        }
    }

    /* Register each sensor as its own hwmon chip instance so
     * monitoring tools show independently-named sensors instead of
     * folding them into one chip with N temp channels. Deliberately
     * NOT devm_* — see lifecycle note at top of file. */
    for (i = 0; i < num_total_sensors; i++) {
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

    pr_info("gddr7_temp: found %s at %s, registered %d hwmon devices "
             "(%d GDDR7 modules + 1 hotspot, %d THERM channels + 1 hotspot)\n",
             active_table->name, pci_name(gpu_dev), num_total_sensors,
             active_table->dqr_num_modules, active_table->therm_num_channels);

    return 0;

err_unregister:
    gpu_unregister_all(i);
err_free_names:
    for (i = 0; i < num_total_sensors; i++)
        kfree(gpu_sensors[i].chip_name);
    /* labels that share chip_name pointers are freed above too */
err_free_sensors:
    kfree(hwmon_devs);
    hwmon_devs = NULL;
    kfree(gpu_sensors);
    gpu_sensors = NULL;
err_unmap_therm:
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
    active_table = NULL;
    return ret;
}

static void __exit gddr7_temp_exit(void)
{
    int i;

    /* Explicit, symmetric teardown for every registered instance —
     * must happen before iounmap/pci teardown so no read callback can
     * race with the resources being freed underneath it. */
    gpu_unregister_all(num_total_sensors);

    /* Free dynamically allocated sensor names. */
    if (gpu_sensors) {
        for (i = 0; i < num_total_sensors; i++)
            kfree(gpu_sensors[i].chip_name);
        kfree(gpu_sensors);
        gpu_sensors = NULL;
    }

    kfree(hwmon_devs);
    hwmon_devs = NULL;

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

    active_table = NULL;
}

module_init(gddr7_temp_init);
module_exit(gddr7_temp_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Sunny Yang <yxh9956@gmail.com>");
MODULE_DESCRIPTION("Read NVIDIA GPU GDDR7 DQR + THERM internal hotspot sensors via hwmon");