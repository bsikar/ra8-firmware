//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host stand-in for the three `ra8_flash_*` entry points that
//! libs/ra8_dfu/src/program_abi.zig externs. The boot and launch C suites link
//! the ra8_dfu_boot archive, which is one compilation unit and so carries the
//! program membrane too, but neither suite ever programs flash. On target the
//! driver resolves these; in the root build's suite path nothing did, so the
//! suites could not link (RA8FW-477).
//!
//! Every entry returns `k_ra8_err_not_supported`, so a suite that does reach
//! one fails on the result instead of passing on a write that never happened.

/// `k_ra8_err_not_supported` (libs/ra8_core/inc/ra8_err.h).
const not_supported: u16 = 0x107;

export fn ra8_flash_open(cfg: ?*const anyopaque) u16 {
    _ = cfg;
    return not_supported;
}

export fn ra8_flash_set_window(low: usize, high: usize) u16 {
    _ = low;
    _ = high;
    return not_supported;
}

export fn ra8_flash_write_block(mram_addr: u32, src: ?[*]const u8, len: u32, world: u8) u16 {
    _ = mram_addr;
    _ = src;
    _ = len;
    _ = world;
    return not_supported;
}
