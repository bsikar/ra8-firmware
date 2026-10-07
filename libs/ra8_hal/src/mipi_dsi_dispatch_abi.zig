//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for ra8_mipi_dsi_attach_handler and the MIPI DSI dispatch ISRs
//! (ra8_mipi_dsi_api.h), RA8FW-658, replacing that block of
//! ra8_mipi_dsi_dispatch.c. The callback and pending-RX globals stay in
//! mipi_dsi_lifecycle_abi.zig (RA8FW-645), where init/deinit reset them.

const common = @import("abi_common.zig");
const dp = @import("internal/mipi_dsi_dispatch.zig");

const tag = "MIPI_DSI";
const base_addr: usize = 0x40346000;

/// `ra8_mipi_dsi_event_fn_t`; the event enum is 1 byte under -fshort-enums.
const EventFn = *const fn (ctx: ?*anyopaque, event: u8, mask: u32) callconv(.c) void;

extern var s_mipi_dsi_event_fn: ?*const anyopaque;
extern var s_mipi_dsi_event_ctx: ?*anyopaque;
extern var s_mipi_dsi_pending_rx_buffer: ?[*]u8;
extern var s_mipi_dsi_pending_rx_len: u16;

const Dsi = struct {
    pub fn read32(_: Dsi, off: u16) u32 {
        return @as(*volatile u32, @ptrFromInt(base_addr + off)).*;
    }
    pub fn write32(_: Dsi, off: u16, value: u32) void {
        @as(*volatile u32, @ptrFromInt(base_addr + off)).* = value;
    }
    pub fn err(_: Dsi, msg: [*:0]const u8) void {
        common.ra8_log_emit_error(tag, msg);
    }
    /// internal_ra8_mipi_dsi_call_user: snapshot fn and ctx, call if set.
    pub fn notify(_: Dsi, event: u8, mask: u32) void {
        const raw = @atomicLoad(?*const anyopaque, &s_mipi_dsi_event_fn, .monotonic);
        const ctx = s_mipi_dsi_event_ctx;
        if (raw) |p| {
            const f: EventFn = @ptrCast(@alignCast(p));
            f(ctx, event, mask);
        }
    }
};

const dsi = Dsi{};

fn pending() dp.PendingRx {
    return .{ .buffer = &s_mipi_dsi_pending_rx_buffer, .len = &s_mipi_dsi_pending_rx_len };
}

export fn ra8_mipi_dsi_attach_handler(f: ?*const anyopaque, ctx: ?*anyopaque) u16 {
    s_mipi_dsi_event_fn = f;
    s_mipi_dsi_event_ctx = ctx;
    return 0;
}

export fn ra8_mipi_dsi_dispatch_seq0() void {
    dp.seq0(dsi);
}

export fn ra8_mipi_dsi_dispatch_seq1() void {
    dp.seq1(dsi);
}

export fn ra8_mipi_dsi_dispatch_video() void {
    dp.video(dsi);
}

export fn ra8_mipi_dsi_dispatch_receive() void {
    dp.receive(dsi, pending());
}

export fn ra8_mipi_dsi_dispatch_fatal() void {
    dp.fatal(dsi);
}

export fn ra8_mipi_dsi_dispatch_phy() void {
    dp.phy(dsi);
}

export fn ra8_mipi_dsi_dispatch() void {
    dp.dispatch(dsi, pending());
}
