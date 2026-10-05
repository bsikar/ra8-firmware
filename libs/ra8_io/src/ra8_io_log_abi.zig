//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI of inc/ra8_io_log.h (RA8FW-654): forwards `ra8_log` bytes into a
//! bound ra8_io stream. Replaces ra8_io_log.c, which is deleted. ra8_core
//! owns the sink callback type; this side implements it.

const tag = "ra8_io_log";

/// ra8_err_t values this unit returns (ra8_err.h).
pub const ok: c_int = 0;
pub const err_not_initialized: c_int = 0x10F;
pub const err_null_ptr: c_int = 0x504;

/// ::ra8_io_stream_t: the bound vtable and its context, both private.
pub const Stream = extern struct {
    iface: ?*const anyopaque,
    ctx: ?*anyopaque,
};

/// ::ra8_log_byte_sink_fn_t.
pub const ByteSink = *const fn (ctx: ?*anyopaque, byte: u8) callconv(.c) void;

extern fn ra8_log_set_byte_sink(sink: ?ByteSink, ctx: ?*anyopaque) void;
extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;
extern fn ra8_io_stream_write(s: *Stream, buf: [*]const u8, len: u32, out_written: ?*u32) c_int;

/// Best-effort byte sink: a write error is dropped so a failing log
/// destination never reaches the logging call site.
fn logByte(ctx: ?*anyopaque, byte: u8) callconv(.c) void {
    const stream: *Stream = @ptrCast(@alignCast(ctx orelse return));
    const one = [1]u8{byte};
    _ = ra8_io_stream_write(stream, &one, 1, null);
}

pub const sink: ByteSink = &logByte;

export fn ra8_io_log_attach(s: ?*Stream) c_int {
    const stream = s orelse {
        ra8_log_emit_error(tag, "s must not be nullptr");
        return err_null_ptr;
    };
    if (stream.iface == null) return err_not_initialized;
    ra8_log_set_byte_sink(sink, stream);
    return ok;
}

export fn ra8_io_log_detach() void {
    ra8_log_set_byte_sink(null, null);
}
