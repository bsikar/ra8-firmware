//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! CANFD channel status, error-flag clear and event dispatch (RA8FW-860,
//! was part of ra8_canfd.c). Exports live in src/canfd_events_abi.zig.

/// CFDC[0]: NCFG +0x0, CTR +0x4, STS +0x8, ERFL +0xC. HUM Ch 41 p 2766/2772.
pub const Chan = extern struct {
    ncfg: u32 = 0,
    ctr: u32 = 0,
    sts: u32 = 0,
    erfl: u32 = 0,
};

comptime {
    if (@sizeOf(Chan) != 16 or @offsetOf(Chan, "sts") != 8 or @offsetOf(Chan, "erfl") != 12) @compileError("Chan layout");
}

/// `ra8_canfd_event_fn_t`.
pub const EventFn = *const fn (ctx: ?*anyopaque, channel: u8, status_mask: u32) callconv(.C) void;

pub const Handler = struct {
    func: ?EventFn = null,
    ctx: ?*anyopaque = null,
};

pub fn status(chan: *const volatile Chan) u32 {
    return chan.sts;
}

/// ERFL is write-0-to-clear: keep every flag outside `mask`.
pub fn clear(chan: *volatile Chan, mask: u32) void {
    chan.erfl = chan.erfl & ~mask;
}

/// Snapshot ERFL, acknowledge it, then hand the snapshot to the handler.
pub fn dispatch(chan: *volatile Chan, channel: u8, handler: Handler) void {
    const mask = chan.erfl;
    chan.erfl = 0;
    if (handler.func) |f| f(handler.ctx, channel, mask);
}
