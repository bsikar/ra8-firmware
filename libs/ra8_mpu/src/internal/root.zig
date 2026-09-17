//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Armv8-M MPU descriptor validation and register encoding, with no hardware
//! access of its own. Everything here is a pure function of a caller-owned
//! region descriptor, so every branch (including the MC/DC pairs the C suite
//! pins down) is reachable from a host test without an MPU in sight.
//!
//! Field positions follow the Arm Cortex-M85 TRM "MPU register summary"; the
//! `ra8_mpu_regs.h` constants are mirrored here rather than imported so the
//! encoders stay independent of the MMIO layer.

const std = @import("std");

// Access permission levels, matching `ra8_mpu_perm_t`. These stay raw `u8`
// rather than a Zig enum: the values arrive from C inside a caller-built
// descriptor, so an out-of-range byte is input to reject, not a value a
// tagged type may hold.
pub const perm_none: u8 = 0;
pub const perm_ro: u8 = 1;
pub const perm_rw: u8 = 2;

// Shareability domain, matching `ra8_mpu_share_t`.
pub const share_non: u8 = 0;
pub const share_outer: u8 = 2;
pub const share_inner: u8 = 3;

/// Smallest region the architecture encodes (`k_ra8_mpu_min_region_size`).
pub const min_region_size: u32 = 32;

// Encoded AP[1:0] values, matching the C's file-local `ra8_mpu_ap_t`.
pub const ap_priv_rw_unpriv_none: u8 = 0;
pub const ap_priv_rw_unpriv_rw: u8 = 1;
pub const ap_priv_ro_unpriv_none: u8 = 2;
pub const ap_priv_ro_unpriv_ro: u8 = 3;
pub const ap_invalid: u8 = 0xFF;

// MPU_CTRL bits.
pub const ctrl_enable: u32 = 1 << 0;
pub const ctrl_hfnmiena: u32 = 1 << 1;
pub const ctrl_privdefena: u32 = 1 << 2;

// MPU_TYPE.DREGION field.
pub const type_dregion_shift: u5 = 8;
pub const type_dregion_mask: u32 = 0x0000FF00;

// MPU_RBAR fields: BASE[31:5] | SH[4:3] | AP[2:1] | XN[0].
pub const rbar_xn_mask: u32 = 0x00000001;
pub const rbar_ap_shift: u5 = 1;
pub const rbar_ap_mask: u32 = 0x00000006;
pub const rbar_sh_shift: u5 = 3;
pub const rbar_sh_mask: u32 = 0x00000018;
pub const rbar_base_mask: u32 = 0xFFFFFFE0;

// MPU_RLAR fields: LIMIT[31:5] | AttrIndx[3:1] | EN[0].
pub const rlar_en_mask: u32 = 0x00000001;
pub const rlar_attridx_shift: u5 = 1;
pub const rlar_attridx_mask: u32 = 0x0000000E;
pub const rlar_limit_mask: u32 = 0xFFFFFFE0;

/// One MPU region descriptor (`ra8_mpu_region_t`), caller-owned.
pub const Region = extern struct {
    base: usize,
    size: u32,
    priv: u8,
    unpriv: u8,
    executable: bool,
    shareable: u8,
    attr_idx: u8,
};

/// Whole-MPU static configuration (`ra8_mpu_cfg_t`), caller-owned.
pub const Config = extern struct {
    regions: ?[*]const Region,
    region_count: u8,
    mair0: u32,
    mair1: u32,
    privdefena: bool,
    hfnmiena: bool,
};

// The two caller-owned layouts are the C ABI. Assert them in pointer-width
// multiples so the same file holds on the 64-bit host and the 32-bit target.
comptime {
    const ptr = @sizeOf(usize);
    std.debug.assert(@offsetOf(Region, "base") == 0);
    std.debug.assert(@offsetOf(Region, "size") == ptr);
    std.debug.assert(@offsetOf(Region, "priv") == ptr + 4);
    std.debug.assert(@offsetOf(Region, "unpriv") == ptr + 5);
    std.debug.assert(@offsetOf(Region, "executable") == ptr + 6);
    std.debug.assert(@offsetOf(Region, "shareable") == ptr + 7);
    std.debug.assert(@offsetOf(Region, "attr_idx") == ptr + 8);
    std.debug.assert(@sizeOf(Region) == std.mem.alignForward(usize, ptr + 9, ptr));

    std.debug.assert(@offsetOf(Config, "regions") == 0);
    std.debug.assert(@offsetOf(Config, "region_count") == ptr);
    std.debug.assert(@offsetOf(Config, "mair0") == ptr + 4);
    std.debug.assert(@offsetOf(Config, "mair1") == ptr + 8);
    std.debug.assert(@offsetOf(Config, "privdefena") == ptr + 12);
    std.debug.assert(@offsetOf(Config, "hfnmiena") == ptr + 13);
}

/// Why a descriptor was rejected. Every arm maps to `k_ra8_err_invalid_arg`
/// at the ABI, exactly as the C did; the split exists so a host test can name
/// which rule fired.
pub const RegionFault = enum {
    ok,
    bad_size,
    misaligned,
    unencodable_perms,
};

/// True when `value` is a positive power of two.
pub fn isPow2(value: u32) bool {
    return (value != 0) and ((value & (value -% 1)) == 0);
}

/// Map a (priv, unpriv) permission pair onto the Armv8-M AP code.
///
/// The AP table cannot express "priv RO + unpriv RW", so that pair (and any
/// byte outside the three declared levels) returns `ap_invalid`.
pub fn encodeAp(priv: u8, unpriv: u8) u8 {
    if (priv == perm_rw and unpriv == perm_none) return ap_priv_rw_unpriv_none;
    if (priv == perm_rw and unpriv == perm_rw) return ap_priv_rw_unpriv_rw;
    if (priv == perm_ro and unpriv == perm_none) return ap_priv_ro_unpriv_none;
    if (priv == perm_ro and unpriv == perm_ro) return ap_priv_ro_unpriv_ro;
    return ap_invalid;
}

/// Validate one descriptor in the C's rule order: size, then base alignment,
/// then permission encodability.
pub fn checkRegion(r: *const Region) RegionFault {
    if (!isPow2(r.size) or r.size < min_region_size) return .bad_size;
    if ((r.base & (@as(usize, r.size) -% 1)) != 0) return .misaligned;
    if (encodeAp(r.priv, r.unpriv) == ap_invalid) return .unencodable_perms;
    return .ok;
}

/// Build the MPU_RBAR word for a descriptor.
pub fn buildRbar(r: *const Region) u32 {
    const base: u32 = @as(u32, @truncate(r.base)) & rbar_base_mask;
    const ap: u32 = (@as(u32, encodeAp(r.priv, r.unpriv)) << rbar_ap_shift) & rbar_ap_mask;
    const sh: u32 = (@as(u32, r.shareable) << rbar_sh_shift) & rbar_sh_mask;
    const xn: u32 = if (r.executable) 0 else rbar_xn_mask;
    return base | sh | ap | xn;
}

/// Build the MPU_RLAR word for a descriptor.
///
/// The limit is inclusive (`base + size - 1`) and truncated to 32 bits before
/// masking, matching the C's `(uint32_t)` cast on a 64-bit host build.
pub fn buildRlar(r: *const Region) u32 {
    const inclusive: u32 = @truncate(r.base +% @as(usize, r.size) -% 1);
    const limit: u32 = inclusive & rlar_limit_mask;
    const idx: u32 = (@as(u32, r.attr_idx) << rlar_attridx_shift) & rlar_attridx_mask;
    return limit | idx | rlar_en_mask;
}

/// Build the MPU_CTRL word a configure request asks for.
pub fn buildCtrl(cfg: *const Config) u32 {
    var ctrl: u32 = ctrl_enable;
    if (cfg.privdefena) ctrl |= ctrl_privdefena;
    if (cfg.hfnmiena) ctrl |= ctrl_hfnmiena;
    return ctrl;
}

/// Decode the implemented region count out of an MPU_TYPE word.
pub fn dregionOf(type_word: u32) u8 {
    return @truncate((type_word & type_dregion_mask) >> type_dregion_shift);
}

// =============================================================================
// Canonical boot memory-attribute map (issue #576)
// =============================================================================

/// Regions in the canonical boot map (`k_ra8_mpu_boot_region_count`).
pub const boot_region_count: u8 = 5;

/// Boot MAIR0: AttrIdx 0 = Normal WB/WA, 1 = Normal non-cacheable, 2 = Device.
pub const boot_mair0: u32 = 0x000444FF;
/// Boot MAIR1: unused, no boot region uses AttrIdx >= 4.
pub const boot_mair1: u32 = 0x00000000;

// Region bases and sizes for the boot map. Region 4 (the shared M85<->M33
// bank) is 640 KiB, deliberately not a power of two: the size-checked public
// setter rejects it, while the base+limit RBAR/RLAR pair encodes it exactly.
pub const boot_base_mram: usize = 0x02000000;
pub const boot_base_sram: usize = 0x22000000;
pub const boot_base_sdram: usize = 0x68000000;
pub const boot_base_peri: usize = 0x40000000;
pub const boot_base_shram: usize = 0x22100000;

pub const boot_size_mram: u32 = 0x00100000;
pub const boot_size_sram: u32 = 0x00100000;
pub const boot_size_sdram: u32 = 0x04000000;
pub const boot_size_peri: u32 = 0x08000000;
pub const boot_size_shram: u32 = 0x000A0000;

/// The canonical 5-region boot memory-attribute map.
///
/// `const`, so it lands in `.rodata` and stays readable from `SystemInit()`
/// before the `.data` copy runs, which is the property the boot caller needs.
pub const boot_regions = [boot_region_count]Region{
    // 0: MRAM code, RO + executable, Normal inner/outer WB/WA cacheable.
    .{
        .base = boot_base_mram,
        .size = boot_size_mram,
        .priv = perm_ro,
        .unpriv = perm_ro,
        .executable = true,
        .shareable = share_non,
        .attr_idx = 0,
    },
    // 1: M85-private SRAM0+1, RW/XN, Normal WB/WA cacheable.
    .{
        .base = boot_base_sram,
        .size = boot_size_sram,
        .priv = perm_rw,
        .unpriv = perm_rw,
        .executable = false,
        .shareable = share_non,
        .attr_idx = 0,
    },
    // 2: External SDRAM, RW/XN, Normal WB/WA cacheable.
    .{
        .base = boot_base_sdram,
        .size = boot_size_sdram,
        .priv = perm_rw,
        .unpriv = perm_rw,
        .executable = false,
        .shareable = share_non,
        .attr_idx = 0,
    },
    // 3: Peripheral window, RW/XN, Device-nGnRE (AttrIdx 2).
    .{
        .base = boot_base_peri,
        .size = boot_size_peri,
        .priv = perm_rw,
        .unpriv = perm_rw,
        .executable = false,
        .shareable = share_non,
        .attr_idx = 2,
    },
    // 4: Shared M85<->M33 SRAM2+3 (640 KiB), RW/XN, Normal non-cacheable
    // (AttrIdx 1) so the mailbox and CPU1 RAM stay coherent with the D-cache.
    .{
        .base = boot_base_shram,
        .size = boot_size_shram,
        .priv = perm_rw,
        .unpriv = perm_rw,
        .executable = false,
        .shareable = share_non,
        .attr_idx = 1,
    },
};

// The boot map must be 5 regions, every base 32-byte aligned and every size a
// non-zero 32-byte multiple: the Armv8-M RBAR/RLAR granularity. Region 4 is
// intentionally 640 KiB (not a power of two).
comptime {
    std.debug.assert(boot_regions.len == boot_region_count);
    const quantum: usize = min_region_size;
    for (boot_regions) |r| {
        std.debug.assert((r.base & (quantum - 1)) == 0);
        std.debug.assert(r.size >= min_region_size);
        std.debug.assert((r.size & (min_region_size - 1)) == 0);
    }
    std.debug.assert(!isPow2(boot_size_shram));
}
