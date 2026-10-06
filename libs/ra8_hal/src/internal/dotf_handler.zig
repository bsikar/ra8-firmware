//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! DOTF event dispatch (RA8FW-834, was part of ra8_dotf.c). The handler state and
//! exports live in src/dotf_handler_abi.zig.

const power = @import("dotf_power.zig");

/// `ra8_dotf_event_fn_t`.
pub const EventFn = *const fn (ctx: ?*anyopaque, channel: u8) callconv(.C) void;

/// Call `f(ctx, channel)` unless the channel is out of range or `f` is null.
pub fn dispatch(f: ?EventFn, ctx: ?*anyopaque, channel: u8) void {
    if (!power.channelInRange(channel)) return;
    if (f) |g| g(ctx, channel);
}
