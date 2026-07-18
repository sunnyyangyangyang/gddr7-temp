// gddr7_temp.c — minimal standalone kernel module.
// Reads the RTX 5090 (GB202) GDDR7 DQR temperature sensors directly via
// ioremap and exposes the hotspot / per-module readout through
// /proc/gddr7_temp. Same registers/decode as the userspace gddr6 tool,
// just read from kernel space instead of via /dev/mem + mmap().
//
// NOTE: reverse-engineered, unofficial. No PLM bypass — this reads a
// register that simply isn't privilege-locked, it doesn't defeat any
// hardware protection. Read-only, no writes to GPU MMIO anywhere.
//
// Safety notes (v2):
//  - The DQR register block is ioremap'd ONCE at module load and
//    iounmap'd at unload. Per-read ioremap/iounmap was removed: doing
//    that on every /proc read caused kernel page table churn and TLB
//    shootdowns on every call, which is a real local-DoS surface since
//    the proc file was world-readable.
//  - Before touching MMIO we call pm_runtime_get_sync() on the GPU's
//    PCI device to force it out of a low-power state (D3hot/D3cold)
//    for the duration of the read, then pm_runtime_put_autosuspend()
//    to let it go back to sleep afterward. Reading BAR space while the
//    device is powered down can produce a PCIe Unsupported Request /
//    Completer Abort, which on some platforms escalates to an MCE.
//  - Any register read that comes back as 0xFFFFFFFF is treated as a
//    failed/invalid bus read and discarded, rather than being decoded
//    as if it were real sensor data (an all-1s DQR validity word
//    would otherwise coincidentally look "valid").
//  - BAR0 length is checked against the address range this module
//    touches before mapping anything.
//  - /proc/gddr7_temp is root-only (0400) as defense in depth, even
//    though the hot path is now just a handful of MMIO reads instead
//    of remap operations.

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/io.h>
#include <linux/pm_runtime.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>

#define NV_VENDOR_ID   0x10de
#define RTX5090_DEV_ID 0x2b85

#define DQR_MODULE0     0x009024C0u
#define DQR_VLD_OFF     0x10u          /* DQR_MODULE0 + 0x10 = validity reg */
#define DQR_STRIDE      0x00004000u
#define DQR_MAX_MODULES 16

/* Total span of BAR0 address space this module ever reads from,
 * starting at DQR_MODULE0. Used both to size the single ioremap and
 * to sanity-check BAR0 is actually big enough before we map it. */
#define DQR_SPAN ((DQR_MAX_MODULES - 1) * DQR_STRIDE + DQR_VLD_OFF + sizeof(u32))

static struct pci_dev *gpu_dev;
static void __iomem *dqr_base;   /* ioremap'd once, covers DQR_SPAN bytes */
static struct proc_dir_entry *proc_entry;

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
     * decode it as sensor data, even though its bit pattern would
     * otherwise pass the validity/poison checks below. */
    if (vld == 0xFFFFFFFFu || dq == 0xFFFFFFFFu)
        return false;

    if (((vld >> 24) & 0xF) != 0xF)          /* not all 4 IC/subp valid */
        return false;
    if ((dq & 0xFFFF0000u) == 0xBADF0000u)   /* poison sentinel */
        return false;

    *out_c = decode_mrcode(dq);
    return true;
}

/* Wake the GPU (best-effort) and read every present module. Fills
 * temps[]/returns count, and *hottest. Always releases the runtime PM
 * reference it took, even on early failure paths. */
static int gddr7_read_modules(int temps[], int *hottest)
{
    int count = 0, hot = -128, p;
    int pm_ret;

    pm_ret = pm_runtime_get_sync(&gpu_dev->dev);
    if (pm_ret < 0) {
        /* Device couldn't be woken (e.g. surprise-removed). Back off
         * the usage counter we just took and report "no data" rather
         * than touching MMIO on a device that may not be there. */
        pm_runtime_put_noidle(&gpu_dev->dev);
        return 0;
    }

    for (p = 0; p < DQR_MAX_MODULES; p++) {
        int c;
        if (!gddr7_read_module(p, &c))
            continue;
        temps[count++] = c;
        if (c > hot)
            hot = c;
    }

    pm_runtime_mark_last_busy(&gpu_dev->dev);
    pm_runtime_put_autosuspend(&gpu_dev->dev);

    *hottest = hot;
    return count;
}

static int gddr7_temp_show(struct seq_file *m, void *v)
{
    int temps[DQR_MAX_MODULES];
    int hottest = 0, n, i;

    if (!gpu_dev || !dqr_base) {
        seq_puts(m, "no compatible GPU found\n");
        return 0;
    }

    n = gddr7_read_modules(temps, &hottest);

    if (n == 0) {
        seq_puts(m, "n/a\n");
        return 0;
    }

    seq_printf(m, "hotspot: %d C\n", hottest);
    for (i = 0; i < n; i++)
        seq_printf(m, "module%d: %d C\n", i, temps[i]);

    return 0;
}

static int gddr7_temp_open(struct inode *inode, struct file *file)
{
    return single_open(file, gddr7_temp_show, NULL);
}

static const struct proc_ops gddr7_temp_fops = {
    .proc_open    = gddr7_temp_open,
    .proc_read    = seq_read,
    .proc_lseek   = seq_lseek,
    .proc_release = single_release,
};

static int __init gddr7_temp_init(void)
{
    resource_size_t bar0_len;
    int ret;

    gpu_dev = pci_get_device(NV_VENDOR_ID, RTX5090_DEV_ID, NULL);
    if (!gpu_dev) {
        pr_warn("gddr7_temp: no RTX 5090 (dev id 0x%04x) found\n", RTX5090_DEV_ID);
        goto out_no_gpu; /* still load; /proc reports "no GPU" */
    }

    bar0_len = pci_resource_len(gpu_dev, 0);
    if (bar0_len < DQR_MODULE0 + DQR_SPAN) {
        pr_err("gddr7_temp: BAR0 too small (0x%llx bytes), refusing to map\n",
               (unsigned long long)bar0_len);
        pci_dev_put(gpu_dev);
        gpu_dev = NULL;
        goto out_no_gpu;
    }

    /* Make sure MMIO decode is actually enabled on this device before
     * we try to read it. Refcounted, so this is safe even if another
     * driver (e.g. nvidia.ko) already has the device enabled. */
    ret = pci_enable_device_mem(gpu_dev);
    if (ret) {
        pr_err("gddr7_temp: pci_enable_device_mem failed: %d\n", ret);
        pci_dev_put(gpu_dev);
        gpu_dev = NULL;
        goto out_no_gpu;
    }

    /* Let runtime PM auto-suspend the device again ~2s after our last
     * read instead of forcing it to stay awake or sleep immediately. */
    pm_runtime_set_autosuspend_delay(&gpu_dev->dev, 2000);
    pm_runtime_use_autosuspend(&gpu_dev->dev);

    dqr_base = ioremap(pci_resource_start(gpu_dev, 0) + DQR_MODULE0, DQR_SPAN);
    if (!dqr_base) {
        pr_err("gddr7_temp: ioremap failed\n");
        pci_disable_device(gpu_dev);
        pci_dev_put(gpu_dev);
        gpu_dev = NULL;
        goto out_no_gpu;
    }

    pr_info("gddr7_temp: found RTX 5090 at %s, BAR0=%pa, mapped %zu bytes at DQR block\n",
             pci_name(gpu_dev), &gpu_dev->resource[0].start, (size_t)DQR_SPAN);

out_no_gpu:
    /* proc entry is root-only: defense in depth. The per-read cost is
     * now just a handful of MMIO reads (no remap), so the old
     * unprivileged-hammering DoS concern is much smaller, but there's
     * no reason to leave it world-readable. */
    proc_entry = proc_create("gddr7_temp", 0400, NULL, &gddr7_temp_fops);
    if (!proc_entry) {
        if (dqr_base)
            iounmap(dqr_base);
        if (gpu_dev) {
            pci_disable_device(gpu_dev);
            pci_dev_put(gpu_dev);
        }
        return -ENOMEM;
    }

    return 0;
}

static void __exit gddr7_temp_exit(void)
{
    proc_remove(proc_entry);

    if (dqr_base)
        iounmap(dqr_base);

    if (gpu_dev) {
        pci_disable_device(gpu_dev);
        pci_dev_put(gpu_dev);
    }
}

module_init(gddr7_temp_init);
module_exit(gddr7_temp_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("you");
MODULE_DESCRIPTION("Read RTX 5090 GDDR7 DQR temperature sensors via ioremap");