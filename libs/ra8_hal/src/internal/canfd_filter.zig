//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! ra8_canfd_filter_set: one acceptance-filter slot at a global index
//! (RA8FW-862, was part of ra8_canfd.c). Exports live in
//! src/canfd_filter_abi.zig.

const afl = @import("canfd_afl.zig");

/// 16 pages x 16 slots (`k_ra8_canfd_afl_total`, `k_ra8_canfd_afl_per_page`).
pub const per_page: u16 = 16;
pub const total: u16 = per_page * 16;
/// `k_ra8_canfd_dlc_max`.
pub const dlc_max: u8 = 15;
/// CFDGAFLM.GAFLLB, bit 29 (`k_ra8_gaflm_bit_gafllb`).
pub const gafllb: u32 = 1 << 29;
/// GAFLP1 DLC[31:28] (`k_ra8_canfd_ptr_shift_dlc`, `k_ra8_canfd_ptr_mask_dlc`).
pub const dlc_shift: u5 = 28;
pub const dlc_mask: u32 = 0xF;

/// `k_ra8_gctr_value_operation`, `k_ra8_gctr_value_reset`.
pub const GlobalMode = enum(u32) { operation = 0, reset = 1 };

pub const Error = error{InvalidArg};

pub fn validate(id: u16, accept_id: u32, dlc: u8) Error!void {
    if (id >= total or dlc > dlc_max) return error.InvalidArg;
    if (accept_id & ~afl.id_ext_mask != 0) return error.InvalidArg;
}

/// RNC0 raised to `id + 1` when `id` is on page 0 and above the current count.
pub fn rnc0With(cfg0: u32, id: u16) ?u32 {
    if (id >= per_page) return null;
    const cur = (cfg0 >> afl.cfg0_rnc0_shift) & afl.cfg0_rnc0_mask;
    const want = (@as(u32, id) + 1) & afl.cfg0_rnc0_mask;
    if (cur >= want) return null;
    return afl.cfg0With(cfg0, @intCast(want));
}

pub fn p1Word(dlc: u8) u32 {
    return ((@as(u32, dlc) & dlc_mask) << dlc_shift) | afl.gaflp1_fdp0;
}

/// Global reset, bump RNC0, write the slot through the unlocked AFL window,
/// back to global operation, re-enable RX FIFO0 (GL_RESET cleared RFE).
/// `hw` provides globalMode, read, write and enableRxFifo0.
pub fn set(hw: anytype, id: u16, accept_id: u32, mask: u32, dlc: u8) Error!u16 {
    try validate(id, accept_id, dlc);
    const reset_err = hw.globalMode(.reset);
    if (reset_err != 0) return reset_err;
    if (rnc0With(hw.read(afl.off_cfg0), id)) |cfg0| hw.write(afl.off_cfg0, cfg0);
    const page: u32 = id / per_page;
    const slot = afl.off_gafl + @as(usize, id % per_page) * afl.gafl_stride;
    hw.write(afl.off_ectr, (page & afl.ectr_aflpn_mask) | afl.ectr_afldae);
    hw.write(slot + 0x0, accept_id);
    hw.write(slot + 0x4, mask | gafllb);
    hw.write(slot + 0x8, 0);
    hw.write(slot + 0xC, p1Word(dlc));
    hw.write(afl.off_ectr, 0);
    const op_err = hw.globalMode(.operation);
    if (op_err != 0) return op_err;
    hw.enableRxFifo0();
    return 0;
}
