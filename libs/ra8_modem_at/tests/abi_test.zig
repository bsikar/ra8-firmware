//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! ABI membrane tests: the caller-owned layouts, every guard in its C order
//! with the message it emits, the full command cycle over a scripted FIFO
//! transport, URC dispatch and registration, the capture path and the
//! test-access helpers the C MC/DC suites link against.

const std = @import("std");
const abi = @import("abi");
const test_helpers = @import("test_helpers");

// -- log sink -------------------------------------------------------------

var log_count: u32 = 0;
var last_message: [64]u8 = @splat(0);

export fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void {
    _ = tag;
    log_count += 1;
    const msg = std.mem.sliceTo(message, 0);
    const n = @min(msg.len, last_message.len - 1);
    @memcpy(last_message[0..n], msg[0..n]);
    last_message[n] = 0;
}

fn lastMessage() []const u8 {
    return std.mem.sliceTo(@as([*:0]const u8, @ptrCast(&last_message)), 0);
}

// -- scripted transport ---------------------------------------------------

const Fifo = struct {
    rx: []const u8 = &.{},
    rx_pos: usize = 0,
    tx: [128]u8 = @splat(0),
    tx_len: usize = 0,
    now: u32 = 0,
    tick_per_rx: u32 = 0,
    tx_fail_at: ?usize = null,
    tx_fail_code: u16 = 0x407,
};

var fifo: Fifo = .{};

fn txByte(ctx: ?*anyopaque, byte: u8) callconv(.c) u16 {
    _ = ctx;
    if (fifo.tx_fail_at) |at| {
        if (fifo.tx_len == at) return fifo.tx_fail_code;
    }
    if (fifo.tx_len < fifo.tx.len) {
        fifo.tx[fifo.tx_len] = byte;
        fifo.tx_len += 1;
    }
    return 0;
}

fn rxByte(ctx: ?*anyopaque, out_byte: *u8) callconv(.c) u16 {
    _ = ctx;
    fifo.now += fifo.tick_per_rx;
    if (fifo.rx_pos >= fifo.rx.len) return 0x10A; // k_ra8_err_no_data
    out_byte.* = fifo.rx[fifo.rx_pos];
    fifo.rx_pos += 1;
    return 0;
}

fn nowMs(ctx: ?*anyopaque) callconv(.c) u32 {
    _ = ctx;
    return fifo.now;
}

var line_buf: [64]u8 = @splat(0);

fn freshCfg() abi.Cfg {
    return .{
        .io = .{ .tx_byte = &txByte, .rx_byte = &rxByte, .now_ms = &nowMs, .ctx = null },
        .line_buf = &line_buf,
        .line_buf_len = line_buf.len,
        .default_timeout_ms = 0,
    };
}

fn setup(script: []const u8) void {
    abi.testResetModule();
    log_count = 0;
    last_message[0] = 0;
    urc_calls = 0;
    urc_last[0] = 0;
    fifo = .{ .rx = script, .tick_per_rx = 1 };
    var cfg = freshCfg();
    std.debug.assert(abi.ra8_modem_at_init(&cfg) == 0);
}

fn txSlice() []const u8 {
    return fifo.tx[0..fifo.tx_len];
}

// -- URC handler ----------------------------------------------------------

var urc_calls: u32 = 0;
var urc_last: [64]u8 = @splat(0);
var urc_ctx_seen: ?*anyopaque = null;

fn urcHandler(line: [*:0]const u8, ctx: ?*anyopaque) callconv(.c) void {
    urc_calls += 1;
    urc_ctx_seen = ctx;
    const msg = std.mem.sliceTo(line, 0);
    const n = @min(msg.len, urc_last.len - 1);
    @memcpy(urc_last[0..n], msg[0..n]);
    urc_last[n] = 0;
}

fn urcLast() []const u8 {
    return std.mem.sliceTo(@as([*:0]const u8, @ptrCast(&urc_last)), 0);
}

// -- layouts --------------------------------------------------------------

test "io and cfg match the caller-owned C layouts" {
    const ptr = @sizeOf(usize);
    try std.testing.expectEqual(ptr * 4, @sizeOf(abi.Io));
    try std.testing.expectEqual(ptr * 6, @sizeOf(abi.Cfg));
    try std.testing.expectEqual(ptr * 4, @offsetOf(abi.Cfg, "line_buf"));
    try std.testing.expectEqual(ptr * 5, @offsetOf(abi.Cfg, "line_buf_len"));
    try std.testing.expectEqual(ptr * 5 + 2, @offsetOf(abi.Cfg, "default_timeout_ms"));
}

// -- init guards ----------------------------------------------------------

test "init rejects a NULL cfg" {
    abi.testResetModule();
    log_count = 0;
    try std.testing.expectEqual(@as(u16, 0x504), abi.ra8_modem_at_init(null));
    try std.testing.expectEqual(@as(u32, 1), log_count);
    try std.testing.expectEqualStrings("cfg", lastMessage());
}

test "init checks line_buf then the three io hooks then the size" {
    abi.testResetModule();
    var cfg = freshCfg();

    cfg.line_buf = null;
    log_count = 0;
    try std.testing.expectEqual(@as(u16, 0x504), abi.ra8_modem_at_init(&cfg));
    try std.testing.expectEqualStrings("cfg->line_buf", lastMessage());

    cfg = freshCfg();
    cfg.io.tx_byte = null;
    try std.testing.expectEqual(@as(u16, 0x504), abi.ra8_modem_at_init(&cfg));
    try std.testing.expectEqualStrings("cfg->io.tx_byte", lastMessage());

    cfg = freshCfg();
    cfg.io.rx_byte = null;
    try std.testing.expectEqual(@as(u16, 0x504), abi.ra8_modem_at_init(&cfg));
    try std.testing.expectEqualStrings("cfg->io.rx_byte", lastMessage());

    cfg = freshCfg();
    cfg.io.now_ms = null;
    try std.testing.expectEqual(@as(u16, 0x504), abi.ra8_modem_at_init(&cfg));
    try std.testing.expectEqualStrings("cfg->io.now_ms", lastMessage());

    cfg = freshCfg();
    cfg.line_buf_len = 15;
    try std.testing.expectEqual(@as(u16, 0x105), abi.ra8_modem_at_init(&cfg));
    try std.testing.expectEqualStrings("line_buf too small", lastMessage());
}

test "init accepts the minimum buffer length and clears the URC table" {
    abi.testResetModule();
    var cfg = freshCfg();
    cfg.line_buf_len = 16;
    try std.testing.expectEqual(@as(u16, 0), abi.ra8_modem_at_init(&cfg));
    try std.testing.expectEqual(
        @as(u16, 0),
        abi.ra8_modem_at_register_unsolicited_handler("+CMTI:", &urcHandler, null),
    );
    try std.testing.expectEqual(@as(u8, 1), abi.testState().urcs.used());
    cfg = freshCfg();
    try std.testing.expectEqual(@as(u16, 0), abi.ra8_modem_at_init(&cfg));
    try std.testing.expectEqual(@as(u8, 0), abi.testState().urcs.used());
}

// -- not-initialized ordering --------------------------------------------

test "every entry point answers not_initialized before its NULL guard" {
    abi.testResetModule();
    log_count = 0;
    try std.testing.expectEqual(@as(u16, 0x10F), abi.ra8_modem_at_send_cmd(null, null, 0));
    try std.testing.expectEqual(@as(u16, 0x10F), abi.ra8_modem_at_send_cmd_capture(null, null, 0, 0));
    try std.testing.expectEqual(
        @as(u16, 0x10F),
        abi.ra8_modem_at_register_unsolicited_handler(null, null, null),
    );
    try std.testing.expectEqual(@as(u16, 0x10F), abi.ra8_modem_at_poll());
    try std.testing.expectEqual(@as(u32, 0), log_count);
}

// -- send_cmd -------------------------------------------------------------

test "send_cmd transmits the command and a trailing CR" {
    setup("AT\r\nOK\r\n");
    try std.testing.expectEqual(@as(u16, 0), abi.ra8_modem_at_send_cmd("AT", null, 100));
    try std.testing.expectEqualStrings("AT\r", txSlice());
}

test "send_cmd rejects a NULL command after init" {
    setup("");
    log_count = 0;
    try std.testing.expectEqual(@as(u16, 0x504), abi.ra8_modem_at_send_cmd(null, null, 10));
    try std.testing.expectEqualStrings("cmd", lastMessage());
}

test "send_cmd returns the transport code when a byte fails to go out" {
    setup("OK\r\n");
    fifo.tx_fail_at = 1;
    try std.testing.expectEqual(@as(u16, 0x407), abi.ra8_modem_at_send_cmd("AT", null, 100));
}

test "send_cmd maps ERROR and the CME and CMS variants to hw_error" {
    const cases = [_][]const u8{ "AT\r\nERROR\r\n", "AT\r\n+CME ERROR: 10\r\n", "AT\r\n+CMS ERROR: 321\r\n", "AT\r\nBUSY\r\n", "AT\r\nNO CARRIER\r\n" };
    for (cases) |script| {
        setup(script);
        try std.testing.expectEqual(@as(u16, 0x204), abi.ra8_modem_at_send_cmd("AT", null, 200));
        try std.testing.expectEqual(@as(u8, 3), @intFromEnum(abi.testState().state));
    }
}

test "send_cmd times out when no final result code arrives" {
    setup("AT\r\n+CSQ: 20,99\r\n");
    try std.testing.expectEqual(@as(u16, 0x203), abi.ra8_modem_at_send_cmd("AT", null, 5));
    try std.testing.expectEqual(@as(u8, 0), @intFromEnum(abi.testState().state));
    try std.testing.expectEqual(@as(u16, 0), abi.testState().accum.len);
}

test "send_cmd waiting on a prefix fails when OK arrives without it" {
    setup("AT+CSQ\r\nOK\r\n");
    log_count = 0;
    try std.testing.expectEqual(@as(u16, 0x204), abi.ra8_modem_at_send_cmd("AT+CSQ", "+CSQ:", 200));
    try std.testing.expectEqualStrings("OK without expected prefix", lastMessage());
}

test "send_cmd waiting on a prefix succeeds when the prefix precedes OK" {
    setup("AT+CSQ\r\n+CSQ: 20,99\r\nOK\r\n");
    try std.testing.expectEqual(@as(u16, 0), abi.ra8_modem_at_send_cmd("AT+CSQ", "+CSQ:", 200));
}

test "send_cmd tolerates a modem with echo disabled" {
    setup("+CSQ: 20,99\r\nOK\r\n");
    try std.testing.expectEqual(@as(u16, 0), abi.ra8_modem_at_send_cmd("AT+CSQ", "+CSQ:", 200));
}

test "send_cmd with an empty expected prefix only needs OK" {
    setup("AT\r\nOK\r\n");
    try std.testing.expectEqual(@as(u16, 0), abi.ra8_modem_at_send_cmd("AT", "", 200));
}

test "send_cmd uses the configured default timeout when the caller passes zero" {
    abi.testResetModule();
    fifo = .{ .rx = "", .tick_per_rx = 1 };
    var cfg = freshCfg();
    cfg.default_timeout_ms = 7;
    try std.testing.expectEqual(@as(u16, 0), abi.ra8_modem_at_init(&cfg));
    try std.testing.expectEqual(@as(u16, 0x203), abi.ra8_modem_at_send_cmd("AT", null, 0));
    try std.testing.expect(fifo.now >= 7);
    try std.testing.expect(fifo.now < 1000);
}

// -- capture --------------------------------------------------------------

test "send_cmd_capture collects payload lines newline separated" {
    setup("AT+CGMR\r\nline one\r\nline two\r\nOK\r\n");
    var out: [64]u8 = @splat(0xAA);
    try std.testing.expectEqual(
        @as(u16, 0),
        abi.ra8_modem_at_send_cmd_capture("AT+CGMR", &out, out.len, 200),
    );
    try std.testing.expectEqualStrings("line one\nline two", std.mem.sliceTo(@as([*:0]const u8, @ptrCast(&out)), 0));
}

test "send_cmd_capture guards cmd then out_buf then buf_len" {
    setup("OK\r\n");
    var out: [8]u8 = @splat(0);
    log_count = 0;
    try std.testing.expectEqual(@as(u16, 0x504), abi.ra8_modem_at_send_cmd_capture(null, &out, out.len, 10));
    try std.testing.expectEqualStrings("cmd", lastMessage());
    try std.testing.expectEqual(@as(u16, 0x504), abi.ra8_modem_at_send_cmd_capture("AT", null, 8, 10));
    try std.testing.expectEqualStrings("out_buf", lastMessage());
    try std.testing.expectEqual(@as(u16, 0x105), abi.ra8_modem_at_send_cmd_capture("AT", &out, 0, 10));
}

test "send_cmd_capture empties the caller buffer before transmitting" {
    setup("");
    var out = [_]u8{ 'j', 'u', 'n', 'k', 0 };
    fifo.tx_fail_at = 0;
    try std.testing.expectEqual(@as(u16, 0x407), abi.ra8_modem_at_send_cmd_capture("AT", &out, out.len, 10));
    try std.testing.expectEqual(@as(u8, 0), out[0]);
}

test "send_cmd_capture truncates silently and stays NUL terminated" {
    setup("AT\r\nabcdefghij\r\nOK\r\n");
    var out: [5]u8 = @splat(0xAA);
    try std.testing.expectEqual(
        @as(u16, 0),
        abi.ra8_modem_at_send_cmd_capture("AT", &out, out.len, 200),
    );
    try std.testing.expectEqualStrings("abcd", std.mem.sliceTo(@as([*:0]const u8, @ptrCast(&out)), 0));
}

// -- URC registration and dispatch ---------------------------------------

test "register guards prefix then fn then the length window" {
    setup("");
    log_count = 0;
    try std.testing.expectEqual(
        @as(u16, 0x504),
        abi.ra8_modem_at_register_unsolicited_handler(null, &urcHandler, null),
    );
    try std.testing.expectEqualStrings("prefix", lastMessage());
    try std.testing.expectEqual(
        @as(u16, 0x504),
        abi.ra8_modem_at_register_unsolicited_handler("+CMTI:", null, null),
    );
    try std.testing.expectEqualStrings("fn", lastMessage());
    try std.testing.expectEqual(
        @as(u16, 0x105),
        abi.ra8_modem_at_register_unsolicited_handler("", &urcHandler, null),
    );
    try std.testing.expectEqual(
        @as(u16, 0x105),
        abi.ra8_modem_at_register_unsolicited_handler("0123456789ABCDEF", &urcHandler, null),
    );
    try std.testing.expectEqual(
        @as(u16, 0),
        abi.ra8_modem_at_register_unsolicited_handler("0123456789ABCDE", &urcHandler, null),
    );
}

test "register replaces a duplicate prefix instead of taking a slot" {
    setup("");
    var ctx_a: u8 = 1;
    var ctx_b: u8 = 2;
    try std.testing.expectEqual(
        @as(u16, 0),
        abi.ra8_modem_at_register_unsolicited_handler("+CMTI:", &urcHandler, &ctx_a),
    );
    try std.testing.expectEqual(
        @as(u16, 0),
        abi.ra8_modem_at_register_unsolicited_handler("+CMTI:", &urcHandler, &ctx_b),
    );
    try std.testing.expectEqual(@as(u8, 1), abi.testState().urcs.used());
}

test "register reports no_mem once all eight slots are taken" {
    setup("");
    var i: u8 = 0;
    while (i < 8) : (i += 1) {
        const prefix = [_:0]u8{ '+', 'A' + i, ':' };
        try std.testing.expectEqual(
            @as(u16, 0),
            abi.ra8_modem_at_register_unsolicited_handler(&prefix, &urcHandler, null),
        );
    }
    try std.testing.expectEqual(
        @as(u16, 0x102),
        abi.ra8_modem_at_register_unsolicited_handler("+ZZ:", &urcHandler, null),
    );
}

test "a URC arriving mid command is dispatched and does not end the wait" {
    setup("AT\r\n+CMTI: \"SM\",3\r\nOK\r\n");
    var ctx: u8 = 42;
    try std.testing.expectEqual(
        @as(u16, 0),
        abi.ra8_modem_at_register_unsolicited_handler("+CMTI:", &urcHandler, &ctx),
    );
    try std.testing.expectEqual(@as(u16, 0), abi.ra8_modem_at_send_cmd("AT", null, 200));
    try std.testing.expectEqual(@as(u32, 1), urc_calls);
    try std.testing.expectEqualStrings("+CMTI: \"SM\",3", urcLast());
    try std.testing.expectEqual(@as(?*anyopaque, @ptrCast(&ctx)), urc_ctx_seen);
}

test "the expected prefix wins over a registered handler for the same line" {
    setup("AT+CSQ\r\n+CSQ: 20,99\r\nOK\r\n");
    _ = abi.ra8_modem_at_register_unsolicited_handler("+CSQ:", &urcHandler, null);
    try std.testing.expectEqual(@as(u16, 0), abi.ra8_modem_at_send_cmd("AT+CSQ", "+CSQ:", 200));
    try std.testing.expectEqual(@as(u32, 0), urc_calls);
}

test "poll drains the FIFO and dispatches URC handlers" {
    setup("+CREG: 1\r\n+CMTI: \"SM\",1\r\n");
    _ = abi.ra8_modem_at_register_unsolicited_handler("+CREG:", &urcHandler, null);
    _ = abi.ra8_modem_at_register_unsolicited_handler("+CMTI:", &urcHandler, null);
    try std.testing.expectEqual(@as(u16, 0), abi.ra8_modem_at_poll());
    try std.testing.expectEqual(@as(u32, 2), urc_calls);
    try std.testing.expectEqualStrings("+CMTI: \"SM\",1", urcLast());
}

test "poll leaves a partial line in the accumulator for the next pass" {
    setup("+CREG");
    try std.testing.expectEqual(@as(u16, 0), abi.ra8_modem_at_poll());
    try std.testing.expectEqual(@as(u16, 5), abi.testState().accum.len);
    try std.testing.expectEqual(@as(u32, 0), urc_calls);
}

// -- test-access helpers -------------------------------------------------

test "priv_modem string helpers are exported with the C contract" {
    try std.testing.expectEqual(@as(u16, 5), abi.priv_modem_str_len("+CSQ:"));
    try std.testing.expectEqual(@as(u8, 1), abi.priv_modem_starts_with("+CSQ: 1", "+CSQ:"));
    try std.testing.expectEqual(@as(u8, 0), abi.priv_modem_starts_with("+CREG: 1", "+CSQ:"));
    try std.testing.expectEqual(@as(u8, 1), abi.priv_modem_str_eq("OK", "OK"));
    try std.testing.expectEqual(@as(u8, 0), abi.priv_modem_str_eq("OK", "OKAY"));
}

test "priv_modem_classify returns every kind through the exported symbol" {
    setup("");
    _ = abi.ra8_modem_at_register_unsolicited_handler("+CMTI:", &urcHandler, null);
    try std.testing.expectEqual(@as(u8, 0), abi.priv_modem_classify("", null, null));
    try std.testing.expectEqual(@as(u8, 1), abi.priv_modem_classify("AT", "AT", null));
    try std.testing.expectEqual(@as(u8, 3), abi.priv_modem_classify("OK", null, null));
    try std.testing.expectEqual(@as(u8, 4), abi.priv_modem_classify("ERROR", null, null));
    try std.testing.expectEqual(@as(u8, 2), abi.priv_modem_classify("+CMTI: 1", null, null));
    try std.testing.expectEqual(@as(u8, 5), abi.priv_modem_classify("+CSQ: 20", null, null));
    try std.testing.expectEqual(@as(u8, 5), abi.priv_modem_classify("+CMTI: 1", null, "+CMTI:"));
}

test "priv_modem_classify short-circuits the echo AND on a NULL echo" {
    setup("");
    try std.testing.expectEqual(@as(u8, 5), abi.priv_modem_classify("AT", null, null));
    try std.testing.expectEqual(@as(u8, 5), abi.priv_modem_classify("AT", "ATZ", null));
}

test "the promoted AND-decisions are exported and pure" {
    var byte: u8 = 0;
    try std.testing.expectEqual(@as(u8, 1), abi.priv_modem_reset_line_should_clear(&byte, 16));
    try std.testing.expectEqual(@as(u8, 0), abi.priv_modem_reset_line_should_clear(null, 16));
    try std.testing.expectEqual(@as(u8, 0), abi.priv_modem_reset_line_should_clear(&byte, 0));
    try std.testing.expectEqual(@as(u8, 1), abi.priv_modem_payload_prefix_matches("+CSQ: 1", "+CSQ:"));
    try std.testing.expectEqual(@as(u8, 0), abi.priv_modem_payload_prefix_matches("+CSQ: 1", ""));
    try std.testing.expectEqual(@as(u8, 0), abi.priv_modem_payload_prefix_matches("+CSQ: 1", null));
    try std.testing.expectEqual(@as(u8, 1), abi.priv_modem_capture_should_clear(&byte, 1));
    try std.testing.expectEqual(@as(u8, 0), abi.priv_modem_capture_should_clear(&byte, 0));
    try std.testing.expectEqual(@as(u8, 0), abi.priv_modem_capture_should_clear(null, 1));
}

test "priv_modem_capture_line appends through the exported symbol" {
    var out: [16]u8 = @splat(0);
    var used: usize = 0;
    abi.priv_modem_capture_line("one", &out, out.len, &used);
    abi.priv_modem_capture_line("two", &out, out.len, &used);
    try std.testing.expectEqualStrings("one\ntwo", out[0..used]);
    abi.priv_modem_capture_line("x", null, 0, &used);
    try std.testing.expectEqual(@as(usize, 7), used);
}

test "test-only C helper adapter forwards into the ABI module" {
    try std.testing.expectEqual(@as(u16, 3), test_helpers.priv_modem_str_len("OK!"));
}

// -- accumulator behaviour through the public API ------------------------

test "an overlong modem line is split rather than overflowing the buffer" {
    abi.testResetModule();
    log_count = 0;
    fifo = .{ .rx = "AT\r\n0123456789abcdefOK\r\n", .tick_per_rx = 0 };
    var cfg = freshCfg();
    cfg.line_buf_len = 16;
    try std.testing.expectEqual(@as(u16, 0), abi.ra8_modem_at_init(&cfg));
    try std.testing.expectEqual(@as(u16, 0), abi.ra8_modem_at_send_cmd("AT", null, 50));
}
