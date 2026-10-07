//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! XSPI status, event handler, dispatch and module-stop (RA8FW-865, was
//! part of ra8_xspi.c). Exports live in src/xspi_events_abi.zig.
//! HUM Ch 44 p 2986.

pub const instance_count: u8 = 2;
/// XSPI0 window and the spacing to XSPI1 (`k_ra8_xspi_stride`).
pub const base: usize = 0x4026_8000;
pub const stride: usize = 0x400;
pub const off_comstt: usize = 0x184;
pub const off_ints: usize = 0x190;
pub const off_intc: usize = 0x194;
/// `k_ra8_xspi_ints_mask_all`.
pub const ints_mask_all: u32 = 0xFFFF_FFFF;
/// MSTPB16 (OSPI0 + DOTF0), MSTPB17 (OSPI1 + DOTF1).
pub const mstp_ids = [_]u16{ (1 << 8) | 16, (1 << 8) | 17 };

pub const EventFn = *const fn (ctx: ?*anyopaque, status_mask: u32) callconv(.c) void;

/// C `ra8_xspi_state_t` (the C typedef went with ra8_xspi.c, RA8FW-868).
pub const State = extern struct {
    func: ?EventFn = null,
    ctx: ?*anyopaque = null,
};

pub fn inRange(instance: u8) bool {
    return instance < instance_count;
}

pub fn instanceBase(instance: u8) ?usize {
    if (!inRange(instance)) return null;
    return base + @as(usize, instance) * stride;
}

pub fn attach(st: *State, func: ?EventFn, ctx: ?*anyopaque) void {
    st.* = .{ .func = func, .ctx = ctx };
}

/// Snapshot INTS, clear every pending flag, then hand the snapshot to the
/// callback when one is attached.
pub fn dispatch(regs: anytype, st: State) void {
    const mask = regs.read(off_ints);
    regs.write(off_intc, ints_mask_all);
    if (st.func) |f| f(st.ctx, mask);
}
