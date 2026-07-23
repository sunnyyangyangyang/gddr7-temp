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

/* VRAM sensor type — determines which decode algorithm to use.
 * When vram_num_modules == 0, the block is absent and vram_type is NONE. */
enum vram_sensor_type {
    VRAM_GDDR7_DQR,   /* GDDR7 DQR MR-code (Blackwell)             */
    VRAM_GDDR6_ADC,   /* GDDR6 ADC 12-bit fixed-point /32          */
    VRAM_NONE,        /* absent                                     */
};

/* THERM sensor type — determines which decode algorithm to use.
 * When therm_num_channels == 0, the block is absent and therm_type is NONE. */
enum therm_sensor_type {
    THERM_BLACKWELL_BJT,  /* Blackwell: bit30 valid, (raw&0xFFFF)/256 °C   */
    THERM_LEGACY_BYTE,    /* Legacy: (raw>>8)&0xFF >= 0x7F invalid sentinel */
    THERM_NONE,           /* absent                                        */
};

struct gpu_offset_table {
    u16 device_id;
    const char *name;

    /* VRAM temperature region (GDDR7 DQR or GDDR6 ADC).
     * vram_num_modules == 0 means this GPU has no VRAM block at all. */
    enum vram_sensor_type vram_type;     /* decode algorithm selector       */
    u32 vram_module0;        /* base offset of module 0 data register   */
    u32 vram_vld_off;        /* offset from module base to validity reg */
    u32 vram_stride;         /* byte stride between consecutive modules */
    int vram_num_modules;    /* number of VRAM modules (0 = absent)     */

    /* THERM internal hotspot region.
     * therm_num_channels == 0 means this GPU has no THERM block at all. */
    enum therm_sensor_type therm_type;   /* decode algorithm selector       */
    u32 therm_ch0;           /* base offset of channel 0 register         */
    u32 therm_ch_stride;     /* byte stride between channels              */
    int therm_num_channels;  /* number of THERM channels (0 = absent)     */
};

/* Data array — definition is in gpu_offsets_generated.h (auto-generated). */
extern const struct gpu_offset_table gpu_tables[];
extern const int gpu_tables_count;

#endif /* GPU_OFFSETS_H */
