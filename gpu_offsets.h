/* gpu_offsets.h — GPU offset table structure + data.
 *
 * The struct definition is hand-written; the gpu_tables[] array data
 * between '{' and '};' is populated by gen_offsets.py from offsets.yaml.
 * Edit offsets.yaml (not this file) to add or change GPU entries.
 *
 * NOTE: All fields are READ-ONLY MMIO offsets. No write offsets exist
 * in this driver — we only read temperature registers, never write to
 * GPU MMIO. If you add a "write" field here, reconsider that invariant. */

#ifndef GPU_OFFSETS_H
#define GPU_OFFSETS_H

#include <linux/types.h>

struct gpu_offset_table {
    u16 device_id;
    const char *name;

    /* GDDR7 DQR temperature region */
    u32 dqr_module0;       /* base offset of module 0 data register   */
    u32 dqr_vld_off;       /* offset from module base to validity reg */
    u32 dqr_stride;        /* byte stride between consecutive modules */
    int dqr_num_modules;   /* number of DQR modules                   */

    /* THERM internal hotspot region */
    u32 therm_ch0;           /* base offset of channel 0 register     */
    u32 therm_ch_stride;     /* byte stride between channels          */
    int therm_num_channels;  /* number of THERM channels              */
};

/* Data array — definition is in gpu_offsets_generated.h (auto-generated). */
extern const struct gpu_offset_table gpu_tables[];
extern const int gpu_tables_count;

#endif /* GPU_OFFSETS_H */
