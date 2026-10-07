//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! GWCA status and event dispatch (RA8FW-847, was part of ra8_eth_gwca.c).
//! The handler state and exports live in src/eth_gwca_events_abi.zig.

/// `r_gwca_regs_t` (ra8_ether_regs.h). The C names are inherited from an
/// older layout: +0x00 is really GWMC and +0x04 GWMS. Kept as named.
pub const Regs = extern struct {
    ctrl: u32,
    sts: u32,
    ie: u32,
    iclr: u32,
};

comptime {
    if (@offsetOf(Regs, "iclr") != 0x0C) @compileError("GWCA ICLR must sit at +0x0C");
}

/// `ra8_eth_gwca_event_fn_t`.
pub const EventFn = *const fn (ctx: ?*anyopaque, status_mask: u32) callconv(.c) void;

/// Write `mask` to ICLR, then clear those bits in STS.
pub fn clearStatus(regs: *volatile Regs, mask: u32) void {
    regs.iclr = mask;
    regs.sts = regs.sts & ~mask;
}

/// Snapshot STS, acknowledge all of it, then hand the mask to `f`.
pub fn dispatch(regs: *volatile Regs, f: ?EventFn, ctx: ?*anyopaque) void {
    const mask = regs.sts;
    regs.iclr = mask;
    regs.sts = 0;
    if (f) |g| g(ctx, mask);
}
