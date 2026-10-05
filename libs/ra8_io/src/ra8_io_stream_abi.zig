//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI of inc/ra8_io_stream.h and ra8_io_stream_bind from
//! inc/ra8_io_stream_backend.h (RA8FW-725): validates the handle, forwards
//! through the bound sink vtable, and renders the no-varargs formatted
//! helpers into small stack buffers. Replaces ra8_io_stream.c, which is
//! deleted.

const Stream = @import("ra8_io_log_abi.zig").Stream;
const Iface = @import("ra8_io_stream_ram_abi.zig").Iface;

const tag = "ra8_io_stream";

/// ra8_err_t values this unit returns (ra8_err.h).
pub const ok: c_int = 0;
pub const err_invalid_arg: c_int = 0x103;
pub const err_invalid_size: c_int = 0x105;
pub const err_not_initialized: c_int = 0x10F;
pub const err_protocol_error: c_int = 0x406;
pub const err_null_ptr: c_int = 0x504;

/// Bounded scan limit for ra8_io_stream_puts.
pub const puts_max: u32 = 65535;
/// Widest min_digits ra8_io_stream_put_hex accepts.
pub const hex_max_digits: u8 = 8;

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;

fn nullPtr(message: [*:0]const u8) c_int {
    ra8_log_emit_error(tag, message);
    return err_null_ptr;
}

/// The bound vtable of a handle, or the error a NULL or unbound handle gets.
fn boundIface(s: ?*Stream) error{ Null, Unbound }!*const Iface {
    const stream = s orelse return error.Null;
    const raw = stream.iface orelse return error.Unbound;
    return @ptrCast(@alignCast(raw));
}

fn validate(s: ?*Stream) c_int {
    _ = boundIface(s) catch |e| return switch (e) {
        error.Null => err_null_ptr,
        error.Unbound => err_not_initialized,
    };
    return ok;
}

export fn ra8_io_stream_bind(stream: ?*Stream, iface: ?*const Iface, context: ?*anyopaque) c_int {
    const s = stream orelse return err_null_ptr;
    const vtable = iface orelse return err_null_ptr;
    if (context == null) return err_null_ptr;
    if (vtable.write == null) return err_invalid_arg;
    s.* = .{ .iface = vtable, .ctx = context };
    return ok;
}

export fn ra8_io_stream_write(s: ?*Stream, buf: ?[*]const u8, len: u32, out_written: ?*u32) c_int {
    const v = validate(s);
    if (v != ok) return v;
    const src = buf orelse return nullPtr("buf must not be nullptr");
    const vtable = boundIface(s) catch unreachable;
    const write = vtable.write orelse return nullPtr("sink write op missing");
    var accepted: u32 = 0;
    const rc = write(s.?.ctx, src, len, &accepted);
    if (accepted > len) return err_protocol_error;
    if (out_written) |out| out.* = accepted;
    if (rc == ok and accepted != len) return err_protocol_error;
    return rc;
}

export fn ra8_io_stream_flush(s: ?*Stream) c_int {
    const v = validate(s);
    if (v != ok) return v;
    const vtable = boundIface(s) catch unreachable;
    const flush = vtable.flush orelse return ok;
    return flush(s.?.ctx);
}

export fn ra8_io_stream_putc(s: ?*Stream, c: u8) c_int {
    const v = validate(s);
    if (v != ok) return v;
    const one = [1]u8{c};
    return ra8_io_stream_write(s, &one, 1, null);
}

export fn ra8_io_stream_puts(s: ?*Stream, str: ?[*]const u8) c_int {
    const v = validate(s);
    if (v != ok) return v;
    const text = str orelse return nullPtr("str must not be nullptr");
    var len: u32 = 0;
    while (len < puts_max) : (len += 1) {
        if (text[len] == 0) return ra8_io_stream_write(s, text, len, null);
    }
    return err_invalid_size;
}

/// Renders `value` in `base`, most significant digit first, padded with
/// '0' up to `min_digits`, and writes the digits to `s`.
fn putDigits(s: ?*Stream, value: u64, base: u8, min_digits: u8) c_int {
    const digits = "0123456789abcdef";
    var reversed: [20]u8 = undefined;
    var n: u32 = 0;
    var rest = value;
    while (true) {
        reversed[n] = digits[@intCast(rest % base)];
        rest /= base;
        n += 1;
        if (rest == 0) break;
    }
    while (n < min_digits) : (n += 1) reversed[n] = '0';
    var out: [20]u8 = undefined;
    for (0..n) |i| out[i] = reversed[n - 1 - i];
    return ra8_io_stream_write(s, &out, n, null);
}

export fn ra8_io_stream_put_u32(s: ?*Stream, value: u32) c_int {
    const v = validate(s);
    if (v != ok) return v;
    return putDigits(s, value, 10, 1);
}

export fn ra8_io_stream_put_u64(s: ?*Stream, value: u64) c_int {
    const v = validate(s);
    if (v != ok) return v;
    return putDigits(s, value, 10, 1);
}

export fn ra8_io_stream_put_hex(s: ?*Stream, value: u32, min_digits: u8) c_int {
    const v = validate(s);
    if (v != ok) return v;
    if (min_digits == 0 or min_digits > hex_max_digits) return err_invalid_arg;
    return putDigits(s, value, 16, min_digits);
}
