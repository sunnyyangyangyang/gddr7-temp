//! gddr7_temp — NVIDIA GPU temperature reader, written in Rust for Linux (issue #14).
//!
// SPDX-License-Identifier: GPL-2.0
/*
 * Reads NVIDIA GPU VRAM and internal THERM temperatures straight from BAR0
 * register space, exposing each sensor as its own hwmon device. Behaviour is
 * a line-for-line port of the former gddr7_temp.c implementation:
 *   - deliberately NOT a PCI driver (the GPU stays bound to nvidia.ko or
 *     nouveau); we only take a reference via pci_get_device()
 *   - two one-shot ioremap()s over the VRAM and THERM sub-spans of BAR0,
 *     volatile 32-bit reads at table-driven offsets
 *   - every sensor is an independent hwmon device with a single temp channel
 *   - explicit register/unregister, never devm_* (the parent device is not
 *     ours; devm callbacks would dangle after rmmod)
 *
 * Rust-for-Linux surface used: module!/Module trait, KBox/KVec,
 * pr_*, Error/Result, Atomic. MMIO is a hand-rolled ioremap() region
 * (see the MMIO region section) — deliberately no kernel::io dependency,
 * because that API was redesigned in the 7.3 dev cycle.
 * FFI (no RFL abstraction exists for these in v7.1 — see
 * .ref-rust/RUST_FEASIBILITY_REPORT.md):
 *   - hwmon_device_register_with_info/unregister plus hand-written repr(C)
 *     ops structs; the layout must match include/linux/hwmon.h field-for-field
 *   - __pm_runtime_resume/__pm_runtime_idle (GPL exports; the inline wrappers
 *     pm_runtime_resume_and_get/put are not linkable symbols)
 */

use core::ffi::{c_char, c_int, CStr};

use kernel::prelude::*;
use kernel::bindings;
use kernel::sync::atomic::{Acquire, Atomic, Release};

/* Raw errno values for the callbacks that must return c_int directly. The
 * prelude only re-exports a subset of errnos as Error values (ENODEV/EINVAL/
 * ENOMEM); these come from linux/err.h via the generated bindings. */
use bindings::{ENODATA as ENODATA_RAW, EOPNOTSUPP as EOPNOTSUPP_RAW};

/* Generated from offsets.yaml — GpuOffsetTable / GPU_TABLES / NV_VENDOR_ID.
 * .rs suffix (not .inc): rust-analyzer's VFS only indexes "rs" files, so an
 * include!() of a .inc file fails with "failed to load file". kbuild does not
 * care about the suffix. */
include!("gpu_tables.rs");

/* ---------------- constants (mirror gddr7_temp.c) ---------------- */

/* v7.1 contract: pci_get_device() takes full-width u32 ids and matches them
 * against struct pci_device_id { __u32 vendor, device; } with the test
 * `id->device == PCI_ANY_ID`, where PCI_ANY_ID is (~0) in a 32-bit context —
 * i.e. 0xFFFFFFFF, NOT the legacy u16-era 0xFFFF. Passing 0x0000FFFF matches
 * nothing (it is neither ANY nor a real device id), so the scan would see
 * zero candidates on v7.x kernels. */
const PCI_ANY_ID: u32 = 0xffff_ffff;
const GPU_MAX_VRAM_MODULES: i32 = 18;
const GPU_MAX_THERM_CHS: i32 = 16;

const THERM_VALID_BIT: u32 = 1 << 30;      /* bit30 marks validity (Blackwell BJT) */
const THERM_RAW_MASK: u32 = 0xFFFF;        /* fixed point, 1/256 °C per LSB       */
const THERM_LEGACY_LIMIT: u32 = 0x7F;      /* >= 127 °C is the invalid sentinel   */

const VRAM_GDDR6_ADC_MASK: u32 = 0xFFF;    /* 12-bit ADC value                    */
const VRAM_GDDR6_DIVISOR: u32 = 32;        /* raw / 32 = degrees Celsius          */

/* hwmon constants — values from include/linux/hwmon.h (v7.1). Two distinct
 * number spaces are in play: chip_info config[] entries are bitmasks, but
 * is_visible/read/read_string receive the raw enum value (bit index) — and v7.1's
 * enum hwmon_temp_attributes starts with hwmon_temp_enable=0, so input=1 and
 * label=21. Comparing against BIT() masks in the callbacks would match nothing. */
const HWMON_TEMP: i32 = 1;          // enum hwmon_sensor_types::hwmon_temp
const HWMON_T_INPUT: u32 = 1;       // hwmon_temp_input (v7.1 enum value)
const HWMON_T_LABEL: u32 = 21;      // hwmon_temp_label (v7.1 enum value)

/* Sensor family (mirrors C enum sensor_family). */
const FAM_VRAM: i32 = 0;     /* idx == -1: max hotspot; idx >= 0: single module   */
const FAM_HOTSPOT: i32 = 1;  /* idx == -1: max hotspot; idx >= 0: single channel  */

/* Static name pools — hwmon chip names must be short identifiers without
 * whitespace so each sensor shows up as its own distinct device. Sized by the
 * GPU_MAX_* hard maxima (the C module kalloc'd these at runtime). */
const VRAM_MOD_NAMES: [&CStr; GPU_MAX_VRAM_MODULES as usize] = [
    c"vrammod0", c"vrammod1", c"vrammod2", c"vrammod3", c"vrammod4", c"vrammod5",
    c"vrammod6", c"vrammod7", c"vrammod8", c"vrammod9", c"vrammod10", c"vrammod11",
    c"vrammod12", c"vrammod13", c"vrammod14", c"vrammod15", c"vrammod16", c"vrammod17",
];
const THERM_CH_NAMES: [&CStr; GPU_MAX_THERM_CHS as usize] = [
    c"thermch0", c"thermch1", c"thermch2", c"thermch3", c"thermch4", c"thermch5",
    c"thermch6", c"thermch7", c"thermch8", c"thermch9", c"thermch10", c"thermch11",
    c"thermch12", c"thermch13", c"thermch14", c"thermch15",
];

/* ---------------- hwmon FFI -----------------------------------------
 * RFL has no hwmon abstraction, so the chip-info/ops structs are declared by
 * hand. Their layout MUST match include/linux/hwmon.h (v7.1) field-for-field:
 * umode_t = u32, enum = i32 (C enums are int), long = i64 on x86_64 (LP64).
 */

#[repr(C)]
struct HwmonOps {
    visible: u32, // umode_t
    is_visible: Option<unsafe extern "C" fn(*const core::ffi::c_void, i32, u32, i32) -> u32>,
    read: Option<unsafe extern "C" fn(*mut bindings::device, i32, u32, i32, *mut i64) -> c_int>,
    /* hwmon.h declares the parameter `const char **str` — writeable outer pointer. */
    read_string:
        Option<unsafe extern "C" fn(*mut bindings::device, i32, u32, i32, *mut *const c_char) -> c_int>,
    write: Option<unsafe extern "C" fn(*mut bindings::device, i32, u32, i32, i64) -> c_int>,
}

#[repr(C)]
struct HwmonChannelInfo {
    ty: i32, // enum hwmon_sensor_types (repr(C) inserts the 4-byte padding)
    config: *const u32, // NULL-terminated list of per-channel attributes
}

#[repr(C)]
struct HwmonChipInfo {
    ops: *const HwmonOps,
    info: *const *const HwmonChannelInfo, // NULL-terminated list of channel infos
}

/* SAFETY: these describe immutable hwmon metadata — the raw pointers target
 * static const data (or hold function pointers), and nothing ever writes through them. */
unsafe impl Sync for HwmonOps {}
unsafe impl Sync for HwmonChannelInfo {}
unsafe impl Sync for HwmonChipInfo {}

unsafe extern "C" {
    /* Returns ERR_PTR(-errno) on failure — decode with is_err_ptr()/ptr_err().
     * Device pointers are typed c_void: in the prebuilt Fedora bindings rmeta,
     * `struct device` comes through as an empty struct, which trips
     * improper_ctypes in extern blocks. */
    fn hwmon_device_register_with_info(
        dev: *mut core::ffi::c_void,
        name: *const c_char,
        drvdata: *mut core::ffi::c_void,
        info: *const HwmonChipInfo,
        extra_groups: *const *const core::ffi::c_void, // const struct attribute_group **
    ) -> *mut core::ffi::c_void;

    fn hwmon_device_unregister(dev: *mut core::ffi::c_void);
}

/* C's IS_ERR()/PTR_ERR() for error-encoded pointers: an error encoding is
 * (void *)(long)-err with 0 < err <= MAX_PTR_ERR (4095). */
const MAX_PTR_ERR: usize = 4095;

#[inline]
fn is_err_ptr(p: *mut core::ffi::c_void) -> bool {
    (p as usize) >= (usize::MAX - MAX_PTR_ERR + 1)
}

#[inline]
fn ptr_err(p: *mut core::ffi::c_void) -> c_int {
    // Bit-preserving reinterpretation: for error pointers the bits are exactly
    // the two's-complement encoding of -err, so truncating to i32 yields it.
    (p as *const core::ffi::c_void as isize) as c_int
}

/* ---------------- runtime PM FFI --------------------------------------
 * RFL has no pm_runtime bindings and the wrappers we need are static inline in
 * linux/pm_runtime.h, so call the exported functions directly (GPL exports;
 * this module is GPL). Semantics replicated from v7.1:
 *   pm_runtime_resume_and_get(dev) = __pm_runtime_resume(dev, RPM_GET_PUT);
 *       on failure → put_noidle (decrement usage_count unless 0, no idle queue)
 *   pm_runtime_put(dev)            = __pm_runtime_idle(dev, RPM_GET_PUT | RPM_ASYNC)
 */
const RPM_ASYNC: c_int = 0x01;
const RPM_GET_PUT: c_int = 0x04;

unsafe extern "C" {
    fn __pm_runtime_resume(dev: *mut core::ffi::c_void, rpmflags: c_int) -> c_int;
    fn __pm_runtime_idle(dev: *mut core::ffi::c_void, rpmflags: c_int) -> c_int;
}

fn pm_resume_and_get(dev: *const bindings::device) -> Result {
    // SAFETY: `dev` is the live GPU device for the whole module lifetime.
    let r = unsafe { __pm_runtime_resume(dev as *mut core::ffi::c_void, RPM_GET_PUT) };
    if r < 0 {
        /* v7.1 put_noidle() is atomic_add_unless(&usage_count, -1, 0). We use
         * __pm_runtime_idle(RPM_GET_PUT) instead: the same decrement plus a
         * possible queued idle check — one extra (harmless) PM cycle on an
         * error path that only happens when resume itself failed. */
        unsafe { let _ = __pm_runtime_idle(dev as *mut _, RPM_GET_PUT); }
        return Err(Error::from_errno(r));
    }
    Ok(())
}

fn pm_put(dev: *const bindings::device) {
    // SAFETY: pairs with a successful pm_resume_and_get().
    unsafe { let _ = __pm_runtime_idle(dev as *mut core::ffi::c_void, RPM_ASYNC | RPM_GET_PUT); }
}

/* ---------------- MMIO region -----------------------------------------
 * One-shot ioremap() of a BAR0 sub-span. Deliberately NOT built on
 * kernel::io (MmioRaw/Mmio): that API was redesigned in the 7.3 dev cycle,
 * while our only needs — volatile 32-bit reads at table-driven offsets plus
 * the ioremap/iounmap lifetime pairing — are done directly on the
 * ioremap() pointer instead. The runtime bounds check (offset + 4 <= span)
 * mirrors the kernel wrapper's try_read32 semantics; ioremap/iounmap are
 * C-stable on every kernel this module builds against (the rawhide CI gate
 * keeps that honest). */

struct IoRegion {
    base: *const u8,        /* ioremap() result; `size` bytes valid  */
    size: usize,              /* span passed to ioremap()             */
}

impl IoRegion {
    /* Fallible 32-bit read with runtime bounds check: same semantics as the
     * kernel's Mmio::try_read32 (EINVAL past the end of the mapping). */
    fn try_read32(&self, offset: usize) -> Result<u32> {
        if offset + 4 > self.size {
            return Err(EINVAL);
        }
        // SAFETY: [base + offset, base + offset + 4) was just bounds-checked
        // against the ioremap span; volatile access per the kernel's
        // ioremap contract (mirrors readl()/ioread32() on x86_64).
        Ok(unsafe { self.base.add(offset).cast::<u32>().read_volatile() })
    }
}

impl Drop for IoRegion {
    fn drop(&mut self) {
        // SAFETY: mirrors the ioremap/iounmap pairing in gddr7_temp.c.
        unsafe { bindings::iounmap(self.base as *mut core::ffi::c_void); }
    }
}

fn map_region(res_start: u64, span: u64) -> Result<IoRegion> {
    // SAFETY: [res_start, res_start + span) is a valid BAR0 sub-range (checked
    // against the BAR size in probe()). ioremap() returns NULL on failure.
    let addr = unsafe { bindings::ioremap(res_start, span as usize) };
    if addr.is_null() {
        return Err(ENOMEM);
    }
    // SAFETY: `addr` is a fresh non-NULL ioremap() result covering `span`
    // bytes; every later access is bounds-checked against that span.
    Ok(IoRegion {
        base: addr as *const u8,
        size: span as usize,
    })
}

/* ---------------- sensor context / module state ----------------------- */

/* Per-sensor context stored as hwmon drvdata (mirrors struct gpu_sensor_ctx).
 * The label is derived from (family, idx) at read time — no storage needed. */
#[derive(Clone, Copy)]
struct SensorCtx {
    family: i32, // FAM_VRAM / FAM_HOTSPOT
    idx: i32,    // -1 for the block's max-hotspot sensor
}

/* RAII owner of the PCI device reference taken in Module::init(). Drop puts
 * the reference and, if we enabled MMIO decode for it, disables it again — so
 * every early return between probe() and full registration cleans up by
 * construction instead of hand-putting on each error branch. */
struct PciDevOwner {
    pdev: *mut bindings::pci_dev, /* held reference                          */
    mem_enabled: bool,            /* pci_enable_device_mem succeeded          */
}

impl Drop for PciDevOwner {
    fn drop(&mut self) {
        // SAFETY: we hold the reference until this point; disable only what we enabled.
        if self.mem_enabled {
            unsafe { bindings::pci_disable_device(self.pdev); }
        }
        unsafe { bindings::pci_dev_put(self.pdev); }
    }
}

impl PciDevOwner {
    /* Raw pointer for code that reaches into the device (BAR0, .dev). */
    fn ptr(&self) -> *mut bindings::pci_dev { self.pdev }
}

/* Everything the hwmon callbacks need at runtime. Leaked to 'static so that
 * the plain extern "C" callbacks (which cannot capture state) can reach it via
 * STATE; freed in destroy_state() after all hwmon devices are unregistered —
 * its drop does the PCI disable/put and the iounmaps. */
struct GpuState {
    dev: *const bindings::device,      /* &pdev->dev — parent for hwmon + PM  */
    table_idx: usize,                        /* index into GPU_TABLES               */
    vram_region: Option<IoRegion>,           /* one-shot ioremap of the VRAM span   */
    therm_region: Option<IoRegion>,          /* one-shot ioremap of the THERM span  */
    ctxs: KVec<SensorCtx>,                   /* per-sensor drvdata (stable after init) */
    n_sensors: u32,
    hwmon_devs: KVec<*mut bindings::device>, /* registered devices, for teardown    */
    /* Declared last so the iounmaps precede PCI disable/put when this is dropped —
     * matching the C exit() order (unregister → iounmap ×2 → disable → put).
     * Never read by name on purpose: it exists purely for its Drop. */
    #[allow(dead_code)]
    pdev_owner: PciDevOwner,           /* held reference + enable state       */
}

/* Global access for the FFI callbacks. Set in Module::init before any hwmon
 * device is registered; cleared in destroy_state() after all are unregistered. */
static STATE: Atomic<*const GpuState> = Atomic::new(core::ptr::null());

/* ---------------- VRAM read path (GDDR7 DQR + GDDR6 ADC) -------------- */

/* Convert raw GDDR7 temp MR-code (bits 23:16 of the DQR data word) to °C. */
fn decode_gddr7_mrcode(raw_dq: u32) -> i32 {
    let mut code = ((raw_dq >> 16) & 0xFF) as i32;
    if code > 80 {
        code = 80;
    }
    if code > 19 {
        (code - 20) * 2
    } else {
        -(40 - code * 2)
    }
}

/* Read one VRAM module in °C. Returns None on any failure (same checks as C). */
fn vram_read_module(st: &GpuState, module_idx: i32) -> Option<i32> {
    let t = &GPU_TABLES[st.table_idx];
    if t.vram_num_modules <= 0 || module_idx < 0 || (module_idx as usize) >= t.vram_num_modules as usize {
        return None;
    }
    let region = st.vram_region.as_ref()?;
    let off = (module_idx as u32).wrapping_mul(t.vram_stride);

    match t.vram_type {
        VramType::Gddr7Dqr => {
            /* GDDR7 DQR: read the validity register first, then data. */
            let vld = region.try_read32((off + t.vram_vld_off) as usize).ok()?;
            let dq = region.try_read32(off as usize).ok()?;

            if vld == 0xFFFF_FFFF || dq == 0xFFFF_FFFF {
                return None;
            }
            if (vld >> 24) & 0xF != 0xF {
                return None; // not all 4 IC/subp valid
            }
            if dq & 0xFFFF_0000 == 0xBADF_0000 {
                return None; // poison sentinel
            }

            Some(decode_gddr7_mrcode(dq))
        }

        VramType::Gddr6Adc => {
            /* GDDR6 ADC: read validity, then the 12-bit ADC value /32. */
            let vld = region.try_read32((off + t.vram_vld_off) as usize).ok()?;
            let raw = region.try_read32(off as usize).ok()?;

            if vld == 0xFFFF_FFFF || raw == 0xFFFF_FFFF {
                return None;
            }
            if (vld >> 24) & 0xF != 0xF {
                return None; // not all 4 IC/subp valid
            }

            Some(((raw & VRAM_GDDR6_ADC_MASK) / VRAM_GDDR6_DIVISOR) as i32)
        }

        VramType::Absent => None,
    }
}

/* Read all VRAM modules into temps[] (°C) and report the hottest. */
fn vram_read_modules(
    st: &GpuState,
    temps: &mut [i32; GPU_MAX_VRAM_MODULES as usize],
    valid_mask: &mut u32,
    hottest: &mut i32,
) -> bool {
    let t = &GPU_TABLES[st.table_idx];
    let mut hot = -128i32;
    let mut mask = 0u32;

    for p in 0..t.vram_num_modules as usize {
        if let Some(c) = vram_read_module(st, p as i32) {
            temps[p] = c;
            mask |= 1 << p;
            if c > hot {
                hot = c;
            }
        }
    }

    *valid_mask = mask;
    *hottest = hot;
    mask != 0
}

/* ---------------- THERM internal hotspot read path -------------------- */

/* Read one THERM channel in millidegrees C. Returns None on any failure. */
fn therm_read_channel(st: &GpuState, ch: i32) -> Option<i32> {
    let t = &GPU_TABLES[st.table_idx];
    if t.therm_num_channels <= 0 || ch < 0 || (ch as usize) >= t.therm_num_channels as usize {
        return None;
    }
    let region = st.therm_region.as_ref()?;
    /* Region base already sits at therm_ch0 (see probe()), so the offset is
     * region-relative — mirroring C's `ioread32(therm_base + ch*stride)`. */
    let off = (ch as u32).wrapping_mul(t.therm_ch_stride);
    let raw = region.try_read32(off as usize).ok()?;

    if raw == 0xFFFF_FFFF {
        return None;
    }

    match t.therm_type {
        ThermType::BlackwellBjt => {
            /* Bit 30 marks validity. Temperature is the lower 16 bits, fixed-point 1/256 °C. */
            if raw & THERM_VALID_BIT == 0 {
                return None;
            }
            Some((((raw & THERM_RAW_MASK) * 1000) / 256) as i32)
        }

        ThermType::LegacyByte => {
            /* Bits 15:8 hold temperature in °C. >= 0x7F is the invalid sentinel. */
            let temp_c = (raw >> 8) & 0xFF;
            if temp_c >= THERM_LEGACY_LIMIT {
                return None;
            }
            Some(temp_c as i32 * 1000)
        }

        ThermType::Absent => None,
    }
}

/* Read all THERM channels into temps_mc[] (millidegrees C), hottest included. */
fn therm_read_channels(
    st: &GpuState,
    temps_mc: &mut [i32; GPU_MAX_THERM_CHS as usize],
    valid_mask: &mut u32,
    hottest_mc: &mut i32,
) -> bool {
    let t = &GPU_TABLES[st.table_idx];
    let mut hot = i32::MIN;
    let mut mask = 0u32;

    for ch in 0..t.therm_num_channels as usize {
        if let Some(mc) = therm_read_channel(st, ch as i32) {
            temps_mc[ch] = mc;
            mask |= 1 << ch;
            if mc > hot {
                hot = mc;
            }
        }
    }

    *valid_mask = mask;
    *hottest_mc = hot;
    mask != 0
}

/* ---------------- shared hwmon callbacks ------------------------------ */

unsafe extern "C" fn hwmon_is_visible(
    _drvdata: *const core::ffi::c_void,
    ty: i32,
    attr: u32,
    channel: i32,
) -> u32 {
    if ty != HWMON_TEMP || channel != 0 {
        return 0;
    }

    match attr {
        HWMON_T_INPUT | HWMON_T_LABEL => 0o444,
        _ => 0,
    }
}

unsafe extern "C" fn hwmon_read(
    dev: *mut bindings::device,
    ty: i32,
    attr: u32,
    channel: i32,
    val: *mut i64,
) -> c_int {
    if ty != HWMON_TEMP || attr != HWMON_T_INPUT || channel != 0 {
        return -(EOPNOTSUPP_RAW as c_int);
    }

    // SAFETY: `dev` is a live hwmon device; drvdata was set at registration and
    // points into GpuState.ctxs, which outlives the device.
    let ctx = unsafe { bindings::dev_get_drvdata(dev) as *const SensorCtx };
    if ctx.is_null() {
        return ENODEV.to_errno();
    }
    // SAFETY: drvdata was set at registration and points into GpuState.ctxs.
    let ctx = unsafe { &*ctx };

    // SAFETY: STATE is non-null for the whole registered window (published before
    // the first registration, cleared only after all devices are unregistered).
    let st_ptr = STATE.load(Acquire);
    if st_ptr.is_null() {
        return ENODEV.to_errno();
    }
    // SAFETY: STATE was published with Release and is valid for the registered window.
    let st = unsafe { &*st_ptr };

    /* Same as C: a failed resume is returned to userspace directly. No put —
     * pm_resume_and_get already balanced the usage counter on failure. */
    if let Err(e) = pm_resume_and_get(st.dev) {
        return e.to_errno();
    }

    let ret = match ctx.family {
        FAM_VRAM => read_vram_temp(st, ctx.idx, val),
        _ => read_therm_temp(st, ctx.idx, val),
    };
    pm_put(st.dev);
    ret
}

unsafe extern "C" fn hwmon_read_string(
    dev: *mut bindings::device,
    ty: i32,
    attr: u32,
    channel: i32,
    str_out: *mut *const c_char,
) -> c_int {
    if ty != HWMON_TEMP || attr != HWMON_T_LABEL || channel != 0 {
        return -(EOPNOTSUPP_RAW as c_int);
    }

    // SAFETY: as in hwmon_read.
    let ctx = unsafe { bindings::dev_get_drvdata(dev) as *const SensorCtx };
    if ctx.is_null() {
        return ENODEV.to_errno();
    }

    /* SAFETY: drvdata points into GpuState.ctxs, which outlives the device. */
    let label = unsafe { sensor_label(&*ctx) };
    // SAFETY: `str_out` is a valid out-param from the hwmon core.
    unsafe { *str_out = label.as_ptr(); }
    0
}

/* Label for a sensor (same strings the C module serves). */
fn sensor_label(ctx: &SensorCtx) -> &'static CStr {
    match ctx.family {
        FAM_VRAM => {
            if ctx.idx < 0 {
                c"hotspot"
            } else {
                VRAM_MOD_NAMES[ctx.idx as usize]
            }
        }
        _ => {
            if ctx.idx < 0 {
                c"hotspot_max"
            } else {
                THERM_CH_NAMES[ctx.idx as usize]
            }
        }
    }
}

fn read_vram_temp(st: &GpuState, idx: i32, val: *mut i64) -> c_int {
    let mut temps = [0i32; GPU_MAX_VRAM_MODULES as usize];
    let mut valid_mask = 0u32;
    let mut hottest = -128i32;

    if !vram_read_modules(st, &mut temps, &mut valid_mask, &mut hottest) {
        return -(ENODATA_RAW as c_int);
    }

    let c = if idx < 0 {
        hottest
    } else if (valid_mask >> idx as u32) & 1 != 0 {
        temps[idx as usize]
    } else {
        return -(ENODATA_RAW as c_int);
    };

    /* hwmon wants millidegrees. */
    // SAFETY: `val` is a valid out-param from the hwmon core.
    unsafe { *val = c as i64 * 1000; }
    0
}

fn read_therm_temp(st: &GpuState, idx: i32, val: *mut i64) -> c_int {
    let mut temps_mc = [0i32; GPU_MAX_THERM_CHS as usize];
    let mut valid_mask = 0u32;
    let mut hottest_mc = i32::MIN;

    if !therm_read_channels(st, &mut temps_mc, &mut valid_mask, &mut hottest_mc) {
        return -(ENODATA_RAW as c_int);
    }

    let mc = if idx < 0 {
        hottest_mc
    } else if (valid_mask >> idx as u32) & 1 != 0 {
        temps_mc[idx as usize]
    } else {
        return -(ENODATA_RAW as c_int);
    };

    /* Already millidegrees. */
    // SAFETY: `val` is a valid out-param from the hwmon core.
    unsafe { *val = mc as i64; }
    0
}

/* Shared across all registrations — the shape is identical for every sensor,
 * only drvdata differs per instance (same design as gddr7_temp.c). */
static HWMON_OPS: HwmonOps = HwmonOps {
    visible: 0,
    is_visible: Some(hwmon_is_visible),
    read: Some(hwmon_read),
    read_string: Some(hwmon_read_string),
    write: None,
};

/* config[] entries are BITMASKS of the attribute enum values above. */
const TEMP_CONFIG: [u32; 2] = [(1 << HWMON_T_INPUT) | (1 << HWMON_T_LABEL), 0];

static TEMP_INFO: HwmonChannelInfo = HwmonChannelInfo {
    ty: HWMON_TEMP,
    config: &TEMP_CONFIG as *const u32,
};

const INFO_LIST: [*const HwmonChannelInfo; 2] = [&TEMP_INFO as *const _, core::ptr::null()];

static CHIP_INFO: HwmonChipInfo = HwmonChipInfo {
    ops: &HWMON_OPS as *const _,
    info: &INFO_LIST as *const _,
};

/* ---------------- registration / teardown ----------------------------- */

fn probe(mut owner: PciDevOwner, table_idx: usize) -> Result<GpuState> {
    /* `owner` carries the PCI reference; every early return below cleans up via
     * its Drop (put always, disable only once MMIO was enabled). */

    // SAFETY: the device is live while we hold the reference; .dev is embedded.
    let dev = unsafe { core::ptr::addr_of!((*owner.ptr()).dev) };

    let table = &GPU_TABLES[table_idx];

    /* Sanity-check the table fields (same as C init): num in [0, MAX], and at
     * least one block must be present. */
    if !(0..=GPU_MAX_VRAM_MODULES).contains(&table.vram_num_modules) {
        pr_err!(
            "gddr7_temp: invalid vram_num_modules {} for {}\n",
            table.vram_num_modules,
            table.name
        );
        return Err(EINVAL); // owner drop → put (not yet enabled)
    }
    if !(0..=GPU_MAX_THERM_CHS).contains(&table.therm_num_channels) {
        pr_err!(
            "gddr7_temp: invalid therm_num_channels {} for {}\n",
            table.therm_num_channels,
            table.name
        );
        return Err(EINVAL); // owner drop → put (not yet enabled)
    }

    let has_vram = table.vram_num_modules > 0;
    let has_therm = table.therm_num_channels > 0;
    if !has_vram && !has_therm {
        pr_err!("gddr7_temp: {} defines no sensors at all\n", table.name);
        return Err(EINVAL); // owner drop → put (not yet enabled)
    }

    /* Compute runtime spans from the table. Skip absent blocks to avoid
     * underflow on (num - 1) when num == 0. */
    let vram_span = if has_vram {
        ((table.vram_num_modules - 1) as u64 * table.vram_stride as u64) + table.vram_vld_off as u64 + 4
    } else {
        0
    };
    let therm_span = if has_therm {
        ((table.therm_num_channels - 1) as u64 * table.therm_ch_stride as u64) + 4
    } else {
        0
    };

    let mut needed: u64 = 0;
    if has_vram {
        needed = table.vram_module0 as u64 + vram_span;
    }
    if has_therm && (table.therm_ch0 as u64 + therm_span) > needed {
        needed = table.therm_ch0 as u64 + therm_span;
    }

    /* BAR0 length via resource[0] (equivalent to pci_resource_len(pdev, 0)). */
    // SAFETY: the device is live while we hold the reference.
    let res = unsafe { &(*owner.ptr()).resource[0] };
    let bar0_len = if res.end < res.start { 0 } else { res.end - res.start + 1 };

    if bar0_len < needed {
        pr_err!("gddr7_temp: BAR0 too small, refusing to map\n");
        return Err(EINVAL); // owner drop → put (not yet enabled)
    }

    /* Make sure MMIO decode is actually enabled before we try to read it.
     * Refcounted — safe even if nvidia.ko already has the device enabled. */
    let r = unsafe { bindings::pci_enable_device_mem(owner.ptr()) };
    if r != 0 {
        pr_err!("gddr7_temp: pci_enable_device_mem failed\n");
        return Err(Error::from_errno(r)); // owner drop → put (not yet enabled)
    }
    owner.mem_enabled = true;

    // SAFETY: the device is live while we hold the reference.
    let bar0_start = unsafe { (*owner.ptr()).resource[0].start };

    /* One-shot ioremap of each BAR0 sub-span (both read-only, same as C). */
    let mut vram_region: Option<IoRegion> = None;
    if has_vram {
        match map_region(bar0_start + table.vram_module0 as u64, vram_span) {
            Ok(region) => vram_region = Some(region),
            Err(_) => {
                pr_err!("gddr7_temp: ioremap of VRAM region failed\n");
                return Err(ENOMEM); // owner drop → disable + put
            }
        }
    }

    let mut therm_region: Option<IoRegion> = None;
    if has_therm {
        match map_region(bar0_start + table.therm_ch0 as u64, therm_span) {
            Ok(region) => therm_region = Some(region),
            Err(_) => {
                pr_err!("gddr7_temp: ioremap of THERM region failed\n");
                // `vram_region` drops here (iounmap), then the owner drop does
                // disable + put.
                return Err(ENOMEM);
            }
        }
    }

    /* Build the sensor context array. Layout (same as C):
     * [VRAM hotspot?][VRAM modules...][THERM channels...][THERM hotspot?]
     * Absent blocks contribute zero sensors (no hotspot either). */
    let n_sensors = ((if has_vram { table.vram_num_modules + 1 } else { 0 })
        + (if has_therm { table.therm_num_channels + 1 } else { 0 })) as usize;

    /* Bare `?` below is safe: on allocation failure the locals (mapped regions)
     * and `owner` all drop, doing iounmap + disable + put. */
    let mut ctxs: KVec<SensorCtx> = KVec::with_capacity(n_sensors, GFP_KERNEL)?;
    if has_vram {
        ctxs.push(SensorCtx { family: FAM_VRAM, idx: -1 }, GFP_KERNEL)?; // max-hotspot first
        for m in 0..table.vram_num_modules {
            ctxs.push(SensorCtx { family: FAM_VRAM, idx: m }, GFP_KERNEL)?;
        }
    }
    if has_therm {
        for ch in 0..table.therm_num_channels {
            ctxs.push(SensorCtx { family: FAM_HOTSPOT, idx: ch }, GFP_KERNEL)?;
        }
        ctxs.push(SensorCtx { family: FAM_HOTSPOT, idx: -1 }, GFP_KERNEL)?; // max-hotspot last
    }

    let hwmon_devs = KVec::with_capacity(n_sensors, GFP_KERNEL)?;

    Ok(GpuState {
        dev,
        table_idx,
        vram_region,
        therm_region,
        ctxs,
        n_sensors: n_sensors as u32,
        hwmon_devs,
        pdev_owner: owner, // ownership transfers into the state
    })
}

/* Register each sensor as its own hwmon chip instance so monitoring tools show
 * independently named sensors instead of one chip with N temp channels.
 * Deliberately NOT devm_* — the parent device is not ours (see gddr7_temp.c). */
fn register_all(st: &mut GpuState) -> Result {
    let table = &GPU_TABLES[st.table_idx];
    let has_vram = table.vram_num_modules > 0;
    let n_vram_sensors = if has_vram { (table.vram_num_modules + 1) as usize } else { 0 };

    /* `ctxs` is fully built in probe() and never grows after this — the
     * per-sensor drvdata pointers below stay valid until destroy_state(). */
    let ctx_base = st.ctxs.as_mut_ptr();

    for i in 0..st.n_sensors as usize {
        /* Chip name from position (mirrors the C sensor-array layout). */
        let name: &CStr = if has_vram && i < n_vram_sensors {
            if i == 0 {
                c"vramhotspot"
            } else {
                VRAM_MOD_NAMES[i - 1]
            }
        } else {
            let j = i - n_vram_sensors;
            if j < table.therm_num_channels as usize {
                THERM_CH_NAMES[j]
            } else {
                c"thermhotspot"
            }
        };

        // SAFETY: drvdata points into st.ctxs, which outlives the hwmon devices.
        let drvdata = unsafe { ctx_base.add(i) as *mut core::ffi::c_void };

        /* SAFETY: `st.dev` is a live parent device; name/drvdata/CHIP_INFO are
         * valid for the whole lifetime of st; extra_groups is NULL. */
        let hd = unsafe {
            hwmon_device_register_with_info(
                st.dev as *mut _,
                name.as_ptr(),
                drvdata,
                &CHIP_INFO,
                core::ptr::null(),
            )
        };

        if is_err_ptr(hd) {
            let e = ptr_err(hd);
            pr_err!("gddr7_temp: hwmon registration failed for sensor {}: {}\n", i, e);
            unregister_registered(st);
            return Err(Error::from_errno(e));
        }

        if st.hwmon_devs.push(hd as *mut _, GFP_KERNEL).is_err() {
            /* Device is registered but untracked — undo it directly. */
            unsafe { hwmon_device_unregister(hd); }
            unregister_registered(st);
            return Err(ENOMEM);
        }
    }

    Ok(())
}

fn unregister_registered(st: &GpuState) {
    /* Explicit, symmetric teardown in reverse registration order. */
    for d in st.hwmon_devs.as_slice().iter().rev() {
        // SAFETY: *d is a device we registered and have not yet unregistered.
        unsafe { hwmon_device_unregister(*d as *mut core::ffi::c_void); }
    }
}

/* Full teardown, in the order of gddr7_temp.c's exit(): unregister all hwmon
 * devices first (no read callback may race with unmapped memory), then dropping
 * GpuState does the iounmaps and — last, via PciDevOwner::drop — PCI
 * disable/put. */
unsafe fn destroy_state(state: *mut GpuState) {
    // SAFETY: `state` is a live leaked allocation we exclusively own here.
    let st = unsafe { &*state };

    unregister_registered(st);

    /* No callbacks can run from here on — clear the global before freeing. */
    STATE.store(core::ptr::null(), Release);

    /* Drop GpuState: drops the IoRegions (iounmap) and both KVecs, then
     * PciDevOwner does pci_disable_device + pci_dev_put last. */
    // SAFETY: `state` came from KBox::leak; no other references exist now.
    unsafe { drop(KBox::from_raw(state)); }
}

/* ---------------- module lifecycle ------------------------------------ */

struct Gddr7Temp {
    /* Leaked 'static GpuState (see its docs). Drop frees it via destroy_state(). */
    state: *const GpuState,
}

/* SAFETY: the sole field is a pointer to the leaked GpuState we exclusively own.
 * All runtime access goes through STATE with Acquire/Release ordering; module
 * init and exit run in process context and never overlap. */
unsafe impl Send for Gddr7Temp {}
unsafe impl Sync for Gddr7Temp {}

impl kernel::Module for Gddr7Temp {
    fn init(_module: &'static ThisModule) -> Result<Self> {
        /* Iterate all NVIDIA devices until one is in our offset table — mirrors
         * the C idiom exactly. pci_get_device() consumes its `from` cursor's
         * reference itself ("The reference count for @from is always decremented
         * if it is not %NULL" — drivers/pci/search.c, v7.1), so no manual puts:
         * each returned candidate carries a fresh reference that either becomes
         * the next cursor (consumed by the following call) or, on match, ours. */
        let mut from: *mut bindings::pci_dev = core::ptr::null_mut(); // ref consumed by the next call
        let mut found: Option<(*mut bindings::pci_dev, usize)> = None;
        loop {
            /* SAFETY: `from` is NULL or live — its reference stays ours until the
             * call below consumes it. */
            let cand = unsafe { bindings::pci_get_device(NV_VENDOR_ID as u32, PCI_ANY_ID, from) };

            if !cand.is_null() {
                match GPU_TABLES.iter().position(|t| t.device_id == unsafe { (*cand).device }) {
                    Some(idx) => {
                        found = Some((cand, idx)); // we own its reference now
                        break;
                    }
                    None => {} // keep scanning — `cand` becomes the next cursor
                }
            }

            if cand.is_null() {
                break; // end of list — the kernel already consumed `from`
            }
            from = cand;
        }

        let (pdev, table_idx) = match found {
            Some(x) => x,
            None => {
                pr_warn!("gddr7_temp: no supported NVIDIA GPU found\n");
                return Err(ENODEV);
            }
        };

        /* Wrap the reference in its RAII owner before probe: from here on every
         * `?` early return cleans up automatically (put always; disable once MMIO
         * was enabled) — including a KBox::new failure below, which drops `state`. */
        let state = probe(PciDevOwner { pdev, mem_enabled: false }, table_idx)?;

        // SAFETY: KBox::new allocated a live GpuState; leak() gives stable 'static storage.
        let leaked = KBox::leak(KBox::new(state, GFP_KERNEL)?);

        /* Publish to the FFI callbacks before any hwmon device exists. */
        STATE.store(leaked as *const GpuState, Release);

        if let Err(e) = register_all(leaked) {
            /* SAFETY: `leaked` is the live allocation we exclusively own. */
            unsafe { destroy_state(leaked as *mut GpuState) };
            return Err(e);
        }

        pr_info!(
            "gddr7_temp: found {} — registered {} hwmon devices\n",
            GPU_TABLES[table_idx].name,
            leaked.n_sensors
        );

        Ok(Gddr7Temp { state: leaked as *const GpuState })
    }
}

impl Drop for Gddr7Temp {
    fn drop(&mut self) {
        // SAFETY: `self.state` is the leaked allocation from init(); we own it.
        unsafe { destroy_state(self.state as *mut GpuState); }
    }
}

/* This kernel's module! macro has no `version:` key, so the .modinfo entry is
 * emitted by hand - same mechanism the macro uses for description. Lets
 * `modinfo` and /sys/module/gddr7_temp/version identify the Rust build and
 * its release, and tell it apart from the legacy C module (which ships no
 * version). The @GDDR7_TEMP_VERSION@ placeholder is substituted by the
 * sed in gddr7_temp-kmod.spec's %install before the akmod source tarball
 * is created, so releasing only ever touches the spec (version +
 * changelog). The same sed rewrites `[u8; __GDDR7_TEMP_VERSION_LEN]` to
 * the real byte-string length (the string must live inside the .modinfo
 * section, which is why this is a fixed-size array rather than a slice
 * or pointer). In the raw source the const below must equal the length
 * of the placeholder string. Local/CI builds compile the raw source and
 * keep the placeholder in modinfo. */
const __GDDR7_TEMP_VERSION_LEN: usize = 29;
#[used(compiler)]
#[link_section = ".modinfo"]
static __GDDR7_TEMP_VERSION_MODINFO: [u8; __GDDR7_TEMP_VERSION_LEN] = *b"version=@GDDR7_TEMP_VERSION@\0";

module! {
    type: Gddr7Temp,
    name: "gddr7_temp",
    authors: ["sunnyyangyangyang"],
    description: "NVIDIA GPU GDDR7 DQR and THERM temperature sensors (Rust for Linux)",
    license: "GPL",
}
