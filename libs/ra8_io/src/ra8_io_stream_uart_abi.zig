//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI of inc/ra8_io_stream_uart.h (RA8FW-699): a stream backend that
//! writes to an SCI channel with ra8_sci_write_polling and flushes with
//! ra8_sci_flush. Replaces ra8_io_stream_uart.c, which is deleted. The
//! vtable is bound through ra8_io_stream_bind, in
//! ra8_io_stream_abi.zig.

const log = @import("ra8_io_log_abi.zig");
const ram = @import("ra8_io_stream_ram_abi.zig");
const Stream = log.Stream;
const Iface = ram.Iface;

const tag = "ra8_io_stream_uart";

pub const ok: c_int = 0;
pub const err_null_ptr: c_int = 0x504;

/// ::ra8_io_stream_uart_state_t: the SCI channel index.
pub const State = extern struct {
    channel: u8,
};

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;
extern fn ra8_io_stream_bind(stream: *Stream, iface: *const Iface, context: ?*anyopaque) c_int;
extern fn ra8_sci_write_polling(channel: u8, data: [*]const u8, len: u32) c_int;
extern fn ra8_sci_flush(channel: u8) c_int;

fn nullPtr(message: [*:0]const u8) c_int {
    ra8_log_emit_error(tag, message);
    return err_null_ptr;
}

/// Publishes len only when the SCI write succeeds; its error passes through.
fn uartWrite(ctx: ?*anyopaque, buf: ?[*]const u8, len: u32, out_written: ?*u32) callconv(.c) c_int {
    const raw = ctx orelse return nullPtr("ctx must not be nullptr");
    const src = buf orelse return nullPtr("buf must not be nullptr");
    const st: *const State = @ptrCast(raw);
    const e = ra8_sci_write_polling(st.channel, src, len);
    if (e != ok) return e;
    if (out_written) |out| out.* = len;
    return ok;
}

fn uartFlush(ctx: ?*anyopaque) callconv(.c) c_int {
    const raw = ctx orelse return nullPtr("ctx must not be nullptr");
    const st: *const State = @ptrCast(raw);
    return ra8_sci_flush(st.channel);
}

pub const iface = Iface{ .write = &uartWrite, .flush = &uartFlush };

export fn ra8_io_stream_uart_init(s: ?*Stream, state: ?*State, channel: u8) c_int {
    const stream = s orelse return nullPtr("s must not be nullptr");
    const st = state orelse return nullPtr("state must not be nullptr");
    st.channel = channel;
    return ra8_io_stream_bind(stream, &iface, st);
}
