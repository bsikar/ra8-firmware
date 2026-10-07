//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for `ra8_c6link_capture_bind`, as `ra8_c6link_capture.h` declares
//! it: a transport whose rows forward to an inner transport and report each
//! transaction and HANDSHAKE edge through `internal/capture_line.zig`.

const line = @import("internal/capture_line.zig");
const Err = @import("abi_err.zig");

/// The public `ra8_c6link_capture.h` view (`c6link_capture_h`, from build.zig).
pub const c = @import("c6link_capture_h");

/// No level reported yet.
const unknown: u8 = 0xFF;

/// A capture's sink as a `put` writer.
const Sink = struct {
    cap: *c.ra8_c6link_capture_t,

    pub fn put(self: Sink, text: []const u8) void {
        const sink = self.cap.sink orelse return;
        sink(self.cap.sink_ctx, text.ptr, @intCast(text.len));
    }
};

fn capture(ctx: ?*anyopaque) *c.ra8_c6link_capture_t {
    return @ptrCast(@alignCast(ctx.?));
}

fn transfer(ctx: ?*anyopaque, tx: [*c]const u8, rx: [*c]u8, len: u16) callconv(.c) c.ra8_err_t {
    const cap = capture(ctx);
    const clock = cap.inner.transfer orelse return Err.invalid_arg;
    const err = clock(cap.inner.ctx, tx, rx, len);
    const sink: Sink = .{ .cap = cap };
    line.frame(sink, cap.seq, "tx", tx[0..len]);
    if (err == Err.ok) line.frame(sink, cap.seq, "rx", rx[0..len]);
    cap.seq +%= 1;
    return err;
}

fn handshakeActive(ctx: ?*anyopaque) callconv(.c) bool {
    const cap = capture(ctx);
    const sample = cap.inner.handshake_active orelse return false;
    const level = sample(cap.inner.ctx);
    const bit: u8 = @intFromBool(level);
    if (cap.handshake != bit) {
        line.edge(Sink{ .cap = cap }, cap.seq, level);
        cap.handshake = bit;
    }
    return level;
}

fn delayMs(ctx: ?*anyopaque, ms: u16) callconv(.c) void {
    const cap = capture(ctx);
    const delay = cap.inner.delay_ms orelse return;
    delay(cap.inner.ctx, ms);
}

/// `ra8_c6link_capture_bind`: wrap `inner` and fill `out` with the capture rows.
pub export fn ra8_c6link_capture_bind(
    cap: ?*c.ra8_c6link_capture_t,
    inner: ?*const c.ra8_c6link_transport_t,
    sink: c.ra8_c6link_capture_sink_t,
    sink_ctx: ?*anyopaque,
    out: ?*c.ra8_c6link_transport_t,
) callconv(.c) c.ra8_err_t {
    const state = cap orelse return Err.null_ptr;
    const wrapped = inner orelse return Err.null_ptr;
    const bound = out orelse return Err.null_ptr;
    if (wrapped.transfer == null or wrapped.handshake_active == null or wrapped.delay_ms == null) return Err.invalid_arg;
    if (sink == null) return Err.invalid_arg;
    state.* = .{ .inner = wrapped.*, .sink = sink, .sink_ctx = sink_ctx, .seq = 0, .handshake = unknown };
    bound.* = .{ .transfer = transfer, .handshake_active = handshakeActive, .delay_ms = delayMs, .ctx = state };
    return Err.ok;
}
