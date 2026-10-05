//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI of inc/ra8_io_stream_ram.h (RA8FW-698): a stream backend that
//! captures written bytes into a caller-owned buffer. Replaces
//! ra8_io_stream_ram.c, which is deleted. The vtable is bound through
//! ra8_io_stream_bind, which stays in ra8_io_stream.c.

const Stream = @import("ra8_io_log_abi.zig").Stream;

const tag = "ra8_io_stream_ram";

/// ra8_err_t values this unit returns (ra8_err.h).
pub const ok: c_int = 0;
pub const err_no_mem: c_int = 0x102;
pub const err_invalid_size: c_int = 0x105;
pub const err_null_ptr: c_int = 0x504;

/// ::ra8_io_stream_ram_state_t: buffer, capacity, bytes captured.
pub const State = extern struct {
    buf: ?[*]u8,
    cap: u32,
    len: u32,
};

pub const WriteFn = *const fn (ctx: ?*anyopaque, buf: ?[*]const u8, len: u32, out_written: ?*u32) callconv(.c) c_int;
pub const FlushFn = *const fn (ctx: ?*anyopaque) callconv(.c) c_int;

/// ::ra8_io_stream_iface_t.
pub const Iface = extern struct {
    write: ?WriteFn,
    flush: ?FlushFn,
};

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;
extern fn ra8_io_stream_bind(stream: *Stream, iface: *const Iface, context: ?*anyopaque) c_int;

fn nullPtr(message: [*:0]const u8) c_int {
    ra8_log_emit_error(tag, message);
    return err_null_ptr;
}

/// Accepts what fits; a short write publishes the accepted count and
/// returns err_no_mem, as the C backend did.
fn ramWrite(ctx: ?*anyopaque, buf: ?[*]const u8, len: u32, out_written: ?*u32) callconv(.c) c_int {
    const raw = ctx orelse return nullPtr("ctx must not be nullptr");
    const src = buf orelse return nullPtr("buf must not be nullptr");
    const st: *State = @ptrCast(@alignCast(raw));
    const room = st.cap -% st.len;
    const fits = len <= room;
    const accepted = if (fits) len else room;
    if (accepted != 0) @memcpy(st.buf.?[st.len..][0..accepted], src[0..accepted]);
    st.len += accepted;
    if (out_written) |out| out.* = accepted;
    return if (fits) ok else err_no_mem;
}

pub const iface = Iface{ .write = &ramWrite, .flush = null };

export fn ra8_io_stream_ram_init(s: ?*Stream, state: ?*State, buf: ?[*]u8, cap: u32) c_int {
    const stream = s orelse return nullPtr("s must not be nullptr");
    const st = state orelse return nullPtr("state must not be nullptr");
    const mem = buf orelse return nullPtr("buf must not be nullptr");
    if (cap == 0) return err_invalid_size;
    st.* = .{ .buf = mem, .cap = cap, .len = 0 };
    return ra8_io_stream_bind(stream, &iface, st);
}

export fn ra8_io_stream_ram_used(state: ?*const State, out_used: ?*u32) c_int {
    const st = state orelse return nullPtr("state must not be nullptr");
    const out = out_used orelse return nullPtr("out_used must not be nullptr");
    out.* = st.len;
    return ok;
}
