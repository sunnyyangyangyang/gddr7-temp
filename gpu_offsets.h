/* gpu_offsets.h — GPU offset table structure definition.
 *
 * This header defines the struct used by both hand-written code and
 * the auto-generated gpu_offsets_generated.h array.
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

#endif /* GPU_OFFSETS_H */
