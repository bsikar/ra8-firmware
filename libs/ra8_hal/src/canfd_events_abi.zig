//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for ra8_canfd_get_status, ra8_canfd_clear_status,
//! ra8_canfd_attach_handler and ra8_canfd_dispatch (RA8FW-860). Logic lives
//! in internal/canfd_events.zig.

const common = @import("abi_common.zig");
const ev = @import("internal/canfd_events.zig");
const tdc = @import("internal/canfd_tdc.zig");

const tag = "CANFD";

/// Registered event handler; replaces the C statics s_canfd_fn/s_canfd_ctx.
var handler: ev.Handler = .{};

fn chan(channel: u8) ?*volatile ev.Chan {
    if (channel >= tdc.channel_bases.len) return null;
    return @ptrFromInt(tdc.channel_bases[channel]);
}

fn nullPtr(msg: [*:0]const u8) u16 {
    common.ra8_log_emit_error(tag, msg);
    return common.k_ra8_err_null_ptr;
}

export fn ra8_canfd_get_status(channel: u8, out_mask: ?*u32) u16 {
    const out = out_mask orelse return nullPtr("out_mask must not be nullptr");
    const c = chan(channel) orelse return nullPtr("channel out of range");
    out.* = ev.status(c);
    return common.k_ra8_ok;
}

export fn ra8_canfd_clear_status(channel: u8, mask: u32) u16 {
    const c = chan(channel) orelse return nullPtr("channel out of range");
    ev.clear(c, mask);
    return common.k_ra8_ok;
}

export fn ra8_canfd_attach_handler(func: ?ev.EventFn, ctx: ?*anyopaque) u16 {
    handler = .{ .func = func, .ctx = ctx };
    return common.k_ra8_ok;
}

export fn ra8_canfd_dispatch(channel: u8) void {
    const c = chan(channel) orelse return;
    ev.dispatch(c, channel, handler);
}
