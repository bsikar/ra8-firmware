//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for `g_ra8_error_sink_log` in
//! `libs/ra8_core/inc/ra8_error_interface.h` (#2875).
//!
//! The production non-fatal sink: a driver reporting a degraded sensor or a
//! CRC mismatch binds this instead of halting through `ra8_fatal_error`.
//!
//! It is a `const` object with no state, which is what lets a driver default
//! its injected sink from a file-scope initializer with no init call to
//! sequence. `ctx` is NULL for exactly that reason; the field exists because
//! the vtable shape also has to serve a stateful sink, such as a test ring
//! buffer.

const sink = @import("error_sink");

extern fn ra8_log_emit_error_val(tag: [*:0]const u8, message: [*:0]const u8, value: u32) void;

/// `ra8_error_interface_t`. `ra8_err_t` is a C23 `enum : uint16_t`, so the
/// report code crosses as a u16 and widens for the log backend, exactly as
/// the C's `(uint32_t)err` cast did.
pub const Interface = extern struct {
    report: ?*const fn (ctx: ?*anyopaque, tag: ?[*:0]const u8, msg: ?[*:0]const u8, err: u16) callconv(.c) void,
    ctx: ?*anyopaque,
};

fn report(ctx: ?*anyopaque, tag: ?[*:0]const u8, msg: ?[*:0]const u8, err: u16) callconv(.c) void {
    _ = ctx;
    ra8_log_emit_error_val(sink.tagOr(tag), sink.messageOr(msg), err);
}

pub export const g_ra8_error_sink_log: Interface = .{
    .report = &report,
    .ctx = null,
};
