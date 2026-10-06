//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! SPI_B per-channel state and the SPEI report rule (RA8FW-894, was part of
//! ra8_spi_b.c). Exports live in src/spi_b_events_abi.zig.

pub const CompleteFn = *const fn (ctx: ?*anyopaque, err_mask: u8) callconv(.c) void;

/// Per-channel handler state; C never sees its layout.
pub const State = extern struct {
    cb: ?CompleteFn,
    ctx: ?*anyopaque,
    initialized: bool,
};

/// SPEI reports only a non-zero error set, and only to an attached handler.
pub fn report(state: State, mask: u8) void {
    if (mask == 0) return;
    const cb = state.cb orelse return;
    cb(state.ctx, mask);
}
