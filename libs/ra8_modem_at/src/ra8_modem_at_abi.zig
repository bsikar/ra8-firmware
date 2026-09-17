//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! ABI membrane for `ra8_modem_at`: the five public entry points from
//! `inc/ra8_modem_at.h`, the eight test-access helpers from
//! `src/ra8_modem_at_internal.h`, the caller-owned `ra8_modem_at_io_t` /
//! `ra8_modem_at_cfg_t` layouts, and the singleton module state the C held in
//! `s_mod`. Guard order, log messages and `ra8_err_t` codes are preserved
//! verbatim.

const std = @import("std");
const builtin = @import("builtin");
const core = @import("internal/root.zig");

/// Subset of `ra8_err_t` this library returns on its own behalf. Transport
/// codes from `io.tx_byte` / `io.rx_byte` pass through untouched.
const Err = struct {
    const ok: u16 = 0;
    const no_mem: u16 = 0x102;
    const invalid_size: u16 = 0x105;
    const not_initialized: u16 = 0x10F;
    const hw_timeout: u16 = 0x203;
    const hw_error: u16 = 0x204;
    const null_ptr: u16 = 0x504;
};

/// `RA8_MODEM_AT_TAG` in the C.
const tag: [*:0]const u8 = "MODEM_AT";

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;

/// `ra8_modem_at_urc_fn_t`.
pub const UrcFn = *const fn (line: [*:0]const u8, ctx: ?*anyopaque) callconv(.c) void;

/// `ra8_modem_at_io_t`: three injected transport hooks plus their context.
pub const Io = extern struct {
    tx_byte: ?*const fn (ctx: ?*anyopaque, byte: u8) callconv(.c) u16 = null,
    rx_byte: ?*const fn (ctx: ?*anyopaque, out_byte: *u8) callconv(.c) u16 = null,
    now_ms: ?*const fn (ctx: ?*anyopaque) callconv(.c) u32 = null,
    ctx: ?*anyopaque = null,
};

/// `ra8_modem_at_cfg_t`: the transport, the caller-owned line buffer and the
/// timeout policy.
pub const Cfg = extern struct {
    io: Io = .{},
    line_buf: ?[*]u8 = null,
    line_buf_len: u16 = 0,
    default_timeout_ms: u16 = 0,
};

comptime {
    const ptr = @sizeOf(usize);
    std.debug.assert(@sizeOf(Io) == ptr * 4);
    std.debug.assert(@offsetOf(Io, "tx_byte") == 0);
    std.debug.assert(@offsetOf(Io, "rx_byte") == ptr);
    std.debug.assert(@offsetOf(Io, "now_ms") == ptr * 2);
    std.debug.assert(@offsetOf(Io, "ctx") == ptr * 3);

    std.debug.assert(@sizeOf(Cfg) == ptr * 6);
    std.debug.assert(@offsetOf(Cfg, "io") == 0);
    std.debug.assert(@offsetOf(Cfg, "line_buf") == ptr * 4);
    std.debug.assert(@offsetOf(Cfg, "line_buf_len") == ptr * 5);
    std.debug.assert(@offsetOf(Cfg, "default_timeout_ms") == ptr * 5 + 2);
}

const Table = core.UrcTable(UrcFn);

/// `s_mod`: the singleton the C kept at file scope.
const Module = struct {
    initialized: u8 = 0,
    cfg: Cfg = .{},
    accum: core.Accumulator = .{},
    state: core.State = .idle,
    urcs: Table = .{},
};

var s_mod: Module = .{};

/// Per-command wait context (`ra8_modem_wait_ctx_t`).
const WaitCtx = struct {
    cmd: ?[*]const u8,
    expected_response: ?[*]const u8,
    capture: ?[*]u8,
    capture_len: usize,
    used: *usize,
    seen_exp: *u8,
};

fn io() *Io {
    return &s_mod.cfg.io;
}

/// `internal_tx_command`: the command bytes then the terminating CR.
fn txCommand(s: [*]const u8) u16 {
    const tx = io().tx_byte orelse {
        ra8_log_emit_error(tag, "cfg->io.tx_byte");
        return Err.null_ptr;
    };
    var i: usize = 0;
    while (s[i] != 0) : (i += 1) {
        const err = tx(io().ctx, s[i]);
        if (err != Err.ok) return err;
    }
    return tx(io().ctx, '\r');
}

fn nowMs() u32 {
    const now = io().now_ms orelse return 0;
    return now(io().ctx);
}

/// `internal_dispatch_urc`: call the first matching handler, if any.
fn dispatchUrc(line: [*]const u8) u8 {
    const slot = s_mod.urcs.match(line) orelse return 0;
    const handler = slot.handler orelse return 0;
    handler(@ptrCast(line), slot.ctx);
    return 1;
}

/// `priv_modem_classify`, module-state half included: the URC table is only
/// consulted when the line is not the prefix the caller is waiting for.
fn classify(line: [*]const u8, cmd_echo: ?[*]const u8, expected_response: ?[*]const u8) core.LineKind {
    if (line[0] == 0) return .empty;
    if (cmd_echo) |echo| {
        if (core.strEq(line, echo) != 0) return .echo;
    }
    var is_err: u8 = 0;
    if (core.classifyFinal(line, &is_err) != 0) {
        return if (is_err != 0) .final_err else .final_ok;
    }
    if (core.payloadPrefixMatches(line, expected_response) == 0) {
        if (dispatchUrc(line) != 0) return .urc;
    }
    return .payload;
}

/// `internal_handle_line`.
fn handleLine(line: [*]const u8, kind: core.LineKind, wc: *const WaitCtx) core.LineAction {
    switch (kind) {
        .empty, .urc => return .cont,
        .echo => {
            s_mod.state = .await_resp;
            return .cont;
        },
        .final_ok => {
            s_mod.state = .done;
            if (wc.seen_exp.* == 0) {
                ra8_log_emit_error(tag, "OK without expected prefix");
                return .done_err;
            }
            return .done_ok;
        },
        .final_err => {
            s_mod.state = .done;
            return .done_err;
        },
        .payload => {
            if (core.payloadPrefixMatches(line, wc.expected_response) != 0) {
                wc.seen_exp.* = 1;
            }
            core.captureLine(line, wc.capture, wc.capture_len, wc.used);
            if (s_mod.state == .await_echo) s_mod.state = .await_resp;
            return .cont;
        },
    }
}

/// `internal_pump_one`: one RX attempt, one accumulated byte, at most one line.
fn pumpOne(wc: *const WaitCtx) core.LineAction {
    const rx = io().rx_byte orelse return .cont;
    var byte: u8 = 0;
    if (rx(io().ctx, &byte) != Err.ok) return .cont;
    const line = s_mod.accum.push(byte) orelse return .cont;
    return handleLine(line, classify(line, wc.cmd, wc.expected_response), wc);
}

/// `internal_wait_response`: drive the accumulator until a final result code
/// or the timeout budget.
fn waitResponse(
    cmd: ?[*]const u8,
    expected_response: ?[*]const u8,
    timeout_ms: u16,
    capture: ?[*]u8,
    capture_len: usize,
) u16 {
    const start = nowMs();
    var used: usize = 0;
    var seen_exp: u8 = core.seenExpSeed(expected_response);
    if (core.captureShouldClear(capture, capture_len) != 0) {
        capture.?[0] = 0;
    }
    s_mod.state = .await_echo;
    const wc = WaitCtx{
        .cmd = cmd,
        .expected_response = expected_response,
        .capture = capture,
        .capture_len = capture_len,
        .used = &used,
        .seen_exp = &seen_exp,
    };
    while (true) {
        switch (pumpOne(&wc)) {
            .done_ok => {
                s_mod.accum.reset();
                return Err.ok;
            },
            .done_err => {
                s_mod.accum.reset();
                return Err.hw_error;
            },
            .cont => {},
        }
        const elapsed = nowMs() -% start;
        if (elapsed >= @as(u32, timeout_ms)) {
            s_mod.state = .idle;
            s_mod.accum.reset();
            return Err.hw_timeout;
        }
    }
}

/// `internal_validate_init_cfg`: NULL checks in the C's order, then the
/// minimum buffer size.
fn validateInitCfg(cfg: ?*const Cfg) u16 {
    const c = cfg orelse {
        ra8_log_emit_error(tag, "cfg");
        return Err.null_ptr;
    };
    if (c.line_buf == null) {
        ra8_log_emit_error(tag, "cfg->line_buf");
        return Err.null_ptr;
    }
    if (c.io.tx_byte == null) {
        ra8_log_emit_error(tag, "cfg->io.tx_byte");
        return Err.null_ptr;
    }
    if (c.io.rx_byte == null) {
        ra8_log_emit_error(tag, "cfg->io.rx_byte");
        return Err.null_ptr;
    }
    if (c.io.now_ms == null) {
        ra8_log_emit_error(tag, "cfg->io.now_ms");
        return Err.null_ptr;
    }
    if (c.line_buf_len < core.min_line_buf_bytes) {
        ra8_log_emit_error(tag, "line_buf too small");
        return Err.invalid_size;
    }
    return Err.ok;
}

pub export fn ra8_modem_at_init(cfg: ?*const Cfg) callconv(.c) u16 {
    const verr = validateInitCfg(cfg);
    if (verr != Err.ok) return verr;
    s_mod.cfg = cfg.?.*;
    s_mod.accum = .{ .buf = s_mod.cfg.line_buf, .cap = s_mod.cfg.line_buf_len, .len = 0 };
    s_mod.state = .idle;
    s_mod.initialized = 1;
    s_mod.urcs.clear();
    s_mod.accum.reset();
    return Err.ok;
}

pub export fn ra8_modem_at_send_cmd(
    cmd: ?[*:0]const u8,
    expected_response: ?[*:0]const u8,
    timeout_ms: u16,
) callconv(.c) u16 {
    if (s_mod.initialized == 0) return Err.not_initialized;
    const command = cmd orelse {
        ra8_log_emit_error(tag, "cmd");
        return Err.null_ptr;
    };
    s_mod.accum.reset();
    const tx_err = txCommand(command);
    if (tx_err != Err.ok) return tx_err;
    return waitResponse(
        command,
        expected_response,
        core.effectiveTimeout(timeout_ms, s_mod.cfg.default_timeout_ms),
        null,
        0,
    );
}

pub export fn ra8_modem_at_send_cmd_capture(
    cmd: ?[*:0]const u8,
    out_buf: ?[*]u8,
    buf_len: usize,
    timeout_ms: u16,
) callconv(.c) u16 {
    if (s_mod.initialized == 0) return Err.not_initialized;
    const command = cmd orelse {
        ra8_log_emit_error(tag, "cmd");
        return Err.null_ptr;
    };
    const out = out_buf orelse {
        ra8_log_emit_error(tag, "out_buf");
        return Err.null_ptr;
    };
    if (buf_len == 0) return Err.invalid_size;
    out[0] = 0;
    s_mod.accum.reset();
    const tx_err = txCommand(command);
    if (tx_err != Err.ok) return tx_err;
    return waitResponse(
        command,
        null,
        core.effectiveTimeout(timeout_ms, s_mod.cfg.default_timeout_ms),
        out,
        buf_len,
    );
}

pub export fn ra8_modem_at_register_unsolicited_handler(
    prefix: ?[*:0]const u8,
    handler: ?UrcFn,
    ctx: ?*anyopaque,
) callconv(.c) u16 {
    if (s_mod.initialized == 0) return Err.not_initialized;
    const pfx = prefix orelse {
        ra8_log_emit_error(tag, "prefix");
        return Err.null_ptr;
    };
    const func = handler orelse {
        ra8_log_emit_error(tag, "fn");
        return Err.null_ptr;
    };
    const plen = core.strLen(pfx);
    if (plen == 0 or plen >= @as(u16, core.max_prefix_len)) return Err.invalid_size;
    if (s_mod.urcs.replace(pfx, func, ctx) != 0) return Err.ok;
    if (s_mod.urcs.insert(pfx, plen, func, ctx) != 0) return Err.ok;
    return Err.no_mem;
}

pub export fn ra8_modem_at_poll() callconv(.c) u16 {
    if (s_mod.initialized == 0) return Err.not_initialized;
    const rx = io().rx_byte orelse return Err.ok;
    while (true) {
        var byte: u8 = 0;
        if (rx(io().ctx, &byte) != Err.ok) break;
        if (s_mod.accum.push(byte)) |line| {
            _ = classify(line, null, null);
        }
    }
    return Err.ok;
}

// --------------------------------------------------------------------------
// Test-access surface (src/ra8_modem_at_internal.h). Kept exported under the
// same names so the untouched C MC/DC suites link against this archive.
// --------------------------------------------------------------------------

pub export fn priv_modem_str_len(s: [*:0]const u8) callconv(.c) u16 {
    return core.strLen(s);
}

pub export fn priv_modem_starts_with(hay: [*:0]const u8, needle: [*:0]const u8) callconv(.c) u8 {
    return core.startsWith(hay, needle);
}

pub export fn priv_modem_str_eq(a: [*:0]const u8, b: [*:0]const u8) callconv(.c) u8 {
    return core.strEq(a, b);
}

pub export fn priv_modem_classify(
    line: [*:0]const u8,
    cmd_echo: ?[*:0]const u8,
    expected_response: ?[*:0]const u8,
) callconv(.c) u8 {
    return @intFromEnum(classify(line, cmd_echo, expected_response));
}

pub export fn priv_modem_capture_line(
    line: [*:0]const u8,
    capture: ?[*]u8,
    capture_len: usize,
    used: ?*usize,
) callconv(.c) void {
    core.captureLine(line, capture, capture_len, used);
}

pub export fn priv_modem_reset_line_should_clear(
    line_buf: ?*const anyopaque,
    line_buf_len: u16,
) callconv(.c) u8 {
    return core.resetLineShouldClear(line_buf, line_buf_len);
}

pub export fn priv_modem_payload_prefix_matches(
    line: [*:0]const u8,
    expected_response: ?[*:0]const u8,
) callconv(.c) u8 {
    return core.payloadPrefixMatches(line, expected_response);
}

pub export fn priv_modem_capture_should_clear(
    capture: ?*const anyopaque,
    capture_len: usize,
) callconv(.c) u8 {
    return core.captureShouldClear(capture, capture_len);
}

// --------------------------------------------------------------------------
// Test accessors: the singleton is private, so the Zig ABI test reads it
// through these instead of a second declaration.
// --------------------------------------------------------------------------

pub fn testState() *Module {
    if (!builtin.is_test) @compileError("test-only accessor");
    return &s_mod;
}

pub fn testResetModule() void {
    if (!builtin.is_test) @compileError("test-only accessor");
    s_mod = .{};
}
