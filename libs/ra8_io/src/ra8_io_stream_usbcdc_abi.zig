//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI of inc/ra8_io_stream_usbcdc.h (RA8FW-700): a write-only stream
//! backend that sends on a USB CDC bulk-IN endpoint through
//! ra8_usb_pal_ep_send. Replaces ra8_io_stream_usbcdc.c, which is deleted.
//! The vtable is bound through ra8_io_stream_bind, which stays in
//! ra8_io_stream.c.

const log = @import("ra8_io_log_abi.zig");
const ram = @import("ra8_io_stream_ram_abi.zig");
const Stream = log.Stream;
const Iface = ram.Iface;

const tag = "ra8_io_stream_usbcdc";

pub const ok: c_int = 0;
pub const err_null_ptr: c_int = 0x504;

/// Largest single ra8_usb_pal_ep_send length (its len is a u16).
pub const max_chunk: u32 = 65535;

/// ::ra8_io_stream_usbcdc_state_t: the bulk-IN endpoint address.
pub const State = extern struct {
    ep_addr: u8,
};

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;
extern fn ra8_io_stream_bind(stream: *Stream, iface: *const Iface, context: ?*anyopaque) c_int;
extern fn ra8_usb_pal_ep_send(ep_addr: u8, data: [*]const u8, len: u16) c_int;

fn nullPtr(message: [*:0]const u8) c_int {
    ra8_log_emit_error(tag, message);
    return err_null_ptr;
}

/// Sends in chunks of at most max_chunk. A send error publishes the bytes
/// already sent and returns that error; success publishes len.
fn usbcdcWrite(ctx: ?*anyopaque, buf: ?[*]const u8, len: u32, out_written: ?*u32) callconv(.c) c_int {
    const raw = ctx orelse return nullPtr("ctx must not be nullptr");
    const src = buf orelse return nullPtr("buf must not be nullptr");
    const st: *const State = @ptrCast(raw);
    var done: u32 = 0;
    while (done < len) {
        const chunk = @min(len - done, max_chunk);
        const e = ra8_usb_pal_ep_send(st.ep_addr, src + done, @intCast(chunk));
        if (e != ok) {
            if (out_written) |out| out.* = done;
            return e;
        }
        done += chunk;
    }
    if (out_written) |out| out.* = len;
    return ok;
}

pub const iface = Iface{ .write = &usbcdcWrite, .flush = null };

export fn ra8_io_stream_usbcdc_init(s: ?*Stream, state: ?*State, ep_addr: u8) c_int {
    const stream = s orelse return nullPtr("s must not be nullptr");
    const st = state orelse return nullPtr("state must not be nullptr");
    st.ep_addr = ep_addr;
    return ra8_io_stream_bind(stream, &iface, st);
}
