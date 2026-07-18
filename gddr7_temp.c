// gddr7_temp.c — minimal standalone kernel module.
// Reads the RTX 5090 (GB202) GDDR7 DQR temperature sensors directly via
// ioremap and exposes the hotspot / per-module readout through
// /proc/gddr7_temp. Same registers/decode as the userspace gddr6 tool,
// just read from kernel space instead of via /dev/mem + mmap().
//
// NOTE: reverse-engineered, unofficial. No PLM bypass — this reads a
// register that simply isn't privilege-locked, it doesn't defeat any
// hardware protection. Read-only, no writes to GPU MMIO anywhere.

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/io.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>

#define NV_VENDOR_ID   0x10de
#define RTX5090_DEV_ID 0x2b85

#define DQR_MODULE0    0x009024C0u
#define DQR_VLD_OFF    0x10u          /* DQR_MODULE0 + 0x10 = validity reg */
#define DQR_STRIDE     0x00004000u
#define DQR_MAX_MODULES 16

static struct pci_dev *gpu_dev;
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
 * Read one 32-bit MMIO register at BAR0+off via a short-lived ioremap.
 * Simple and safe for a low-frequency polling use case; not meant for
 * hot-path reads.
 */
static int read_bar0_reg(resource_size_t bar0_phys, u32 off, u32 *out)
{
    void __iomem *map;
    resource_size_t phys = bar0_phys + off;

    map = ioremap(phys, sizeof(u32));
    if (!map)
        return -ENOMEM;

    *out = ioread32(map);
    iounmap(map);
    return 0;
}

/* Fill temps[] for up to DQR_MAX_MODULES; return count found + hottest. */
static int gddr7_read_modules(resource_size_t bar0_phys, int temps[], int *hottest)
{
    int count = 0, hot = -128;
    int p;

    for (p = 0; p < DQR_MAX_MODULES; p++) {
        u32 off = DQR_MODULE0 + (u32)p * DQR_STRIDE;
        u32 vld = 0, dq = 0;
        int all_valid, poison, c;

        if (read_bar0_reg(bar0_phys, off + DQR_VLD_OFF, &vld) != 0)
            continue;
        if (read_bar0_reg(bar0_phys, off, &dq) != 0)
            continue;

        all_valid = (((vld >> 24) & 0xF) == 0xF);
        poison    = ((dq & 0xFFFF0000u) == 0xBADF0000u);
        if (!all_valid || poison)
            continue;

        c = decode_mrcode(dq);
        temps[count++] = c;
        if (c > hot)
            hot = c;
    }

    *hottest = hot;
    return count;
}

static int gddr7_temp_show(struct seq_file *m, void *v)
{
    resource_size_t bar0_phys;
    int temps[DQR_MAX_MODULES];
    int hottest = 0, n, i;

    if (!gpu_dev) {
        seq_puts(m, "no compatible GPU found\n");
        return 0;
    }

    bar0_phys = pci_resource_start(gpu_dev, 0);
    n = gddr7_read_modules(bar0_phys, temps, &hottest);

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
    gpu_dev = pci_get_device(NV_VENDOR_ID, RTX5090_DEV_ID, NULL);
    if (!gpu_dev) {
        pr_warn("gddr7_temp: no RTX 5090 (dev id 0x%04x) found\n", RTX5090_DEV_ID);
        /* Still load; /proc entry will just report "no compatible GPU". */
    } else {
        pr_info("gddr7_temp: found RTX 5090 at %s, BAR0=%pa\n",
                 pci_name(gpu_dev), &gpu_dev->resource[0].start);
    }

    proc_entry = proc_create("gddr7_temp", 0444, NULL, &gddr7_temp_fops);
    if (!proc_entry) {
        if (gpu_dev)
            pci_dev_put(gpu_dev);
        return -ENOMEM;
    }

    return 0;
}

static void __exit gddr7_temp_exit(void)
{
    proc_remove(proc_entry);
    if (gpu_dev)
        pci_dev_put(gpu_dev);
}

module_init(gddr7_temp_init);
module_exit(gddr7_temp_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("you");
MODULE_DESCRIPTION("Read RTX 5090 GDDR7 DQR temperature sensors via ioremap");
