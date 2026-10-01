//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for `libs/ra8_core/inc/ra8_log.h`.
//!
//! The eight emit entry points keep WEAK linkage, which the C spelled
//! `[[gnu::weak]]`. That is not decoration: eleven test fixtures and two
//! host tools define their own `ra8_log_emit_error` and friends to capture
//! log lines, and a strong definition here would turn every one of those
//! links into a duplicate-symbol error.
//!
//! `ra8_scb_trace_enabled` stays a call into ra8_scb, Zig too as of the
//! fault block, the same delegation the SysTick port makes for the
//! DEMCR unlock.

const itm = @import("log_itm");
const line = @import("log_line");
const err_names = @import("log_err_names");

/// `ra8_log_byte_sink_fn_t`: where a redirected line's bytes go.
const ByteSink = *const fn (ctx: ?*anyopaque, byte: u8) callconv(.c) void;

var byte_sink: ?ByteSink = null;
var byte_sink_ctx: ?*anyopaque = null;

/// Level names, exactly as the C wrote them into each line.
const level = struct {
    pub const err: []const u8 = "ERROR";
    pub const warn: []const u8 = "WARN";
    pub const info: []const u8 = "INFO";
    pub const debug: []const u8 = "DEBUG";
};

pub export fn ra8_log_set_byte_sink(sink: ?ByteSink, ctx: ?*anyopaque) callconv(.c) void {
    byte_sink = sink;
    byte_sink_ctx = ctx;
}

/// ITM is brought up by the debugger when it attaches, so the default
/// backend has nothing to initialise. A UART-backed override configures its
/// own channel in its own `ra8_log_init`.
pub export fn ra8_log_init() callconv(.c) void {}

/// An installed sink needs no debugger and is always ready. Only with no
/// sink do the architectural ITM checks run, and off target there is no ITM,
/// so a sink is the whole readiness condition there.
fn ready() bool {
    if (byte_sink != null) return true;
    return itm.ready();
}

fn put(byte: u8) void {
    if (byte_sink) |sink| {
        sink(byte_sink_ctx, byte);
        return;
    }
    itm.put(byte);
}

/// Length of a NUL-terminated caller string, as a slice over its bytes.
fn span(text: [*:0]const u8) []const u8 {
    var end: usize = 0;
    while (text[end] != 0) end += 1;
    return text[0..end];
}

fn emit(level_name: []const u8, tag: [*:0]const u8, message: [*:0]const u8) void {
    if (!ready()) return;
    line.plain(put, level_name, span(tag), span(message));
}

fn emitUnsigned(
    level_name: []const u8,
    tag: [*:0]const u8,
    message: [*:0]const u8,
    value: u32,
) void {
    if (!ready()) return;
    line.withUnsigned(put, level_name, span(tag), span(message), value);
}

fn emitSigned(
    level_name: []const u8,
    tag: [*:0]const u8,
    message: [*:0]const u8,
    value: i32,
) void {
    if (!ready()) return;
    line.withSigned(put, level_name, span(tag), span(message), value);
}

// ---- plain string log ---------------------------------------------------

fn emitError(tag: [*:0]const u8, message: [*:0]const u8) callconv(.c) void {
    emit(level.err, tag, message);
}

fn emitWarn(tag: [*:0]const u8, message: [*:0]const u8) callconv(.c) void {
    emit(level.warn, tag, message);
}

fn emitInfo(tag: [*:0]const u8, message: [*:0]const u8) callconv(.c) void {
    emit(level.info, tag, message);
}

fn emitDebug(tag: [*:0]const u8, message: [*:0]const u8) callconv(.c) void {
    emit(level.debug, tag, message);
}

// ---- string + value log -------------------------------------------------

fn emitErrorVal(tag: [*:0]const u8, message: [*:0]const u8, value: u32) callconv(.c) void {
    emitUnsigned(level.err, tag, message, value);
}

fn emitWarnVal(tag: [*:0]const u8, message: [*:0]const u8, value: u32) callconv(.c) void {
    emitUnsigned(level.warn, tag, message, value);
}

fn emitInfoVal(tag: [*:0]const u8, message: [*:0]const u8, value: u32) callconv(.c) void {
    emitUnsigned(level.info, tag, message, value);
}

fn emitDebugVal(tag: [*:0]const u8, message: [*:0]const u8, value: i32) callconv(.c) void {
    emitSigned(level.debug, tag, message, value);
}

/// Every weakly-exported entry point, paired with the name it takes.
const weak_surface = .{
    .{ "ra8_log_emit_error", &emitError },
    .{ "ra8_log_emit_warn", &emitWarn },
    .{ "ra8_log_emit_info", &emitInfo },
    .{ "ra8_log_emit_debug", &emitDebug },
    .{ "ra8_log_emit_error_val", &emitErrorVal },
    .{ "ra8_log_emit_warn_val", &emitWarnVal },
    .{ "ra8_log_emit_info_val", &emitInfoVal },
    .{ "ra8_log_emit_debug_val", &emitDebugVal },
};

comptime {
    for (weak_surface) |entry| {
        @export(entry[1], .{ .name = entry[0], .linkage = .weak });
    }
}

// ---- ra8_err_to_str -----------------------------------------------------

pub export fn ra8_err_to_str(err: c_int) callconv(.c) [*:0]const u8 {
    return err_names.lookup(err).ptr;
}
