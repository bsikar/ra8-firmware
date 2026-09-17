//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Tests for the C ABI membrane. The `ra8_eth` seam and the four log entry
//! points are declared extern by the library, so this file exports its own
//! versions of them: the same link-time substitution the host C build makes
//! against the fake Ethernet fixture, which means the membrane under test is
//! byte-identical to the shipped one.
//!
//! Capturing the handler the PAL installs also reaches the dispatch guard the
//! C host suite could only cover cross-compiled: both conditions of
//! `event_fn && pal_mask` can be varied here directly.

const std = @import("std");
const abi = @import("abi");

const ok: u16 = 0;
const no_mem: u16 = 0x102;
const invalid_arg: u16 = 0x103;
const invalid_state: u16 = 0x104;
const no_data: u16 = 0x10A;
const hw_init_failed: u16 = 0x201;
const null_ptr: u16 = 0x504;

const frame_max: u16 = 1518;

const EthHandler = ?*const fn (ctx: ?*anyopaque, status_mask: u32) callconv(.c) void;

var eth_init_rc: u16 = ok;
var eth_deinit_rc: u16 = ok;
var eth_init_calls: u32 = 0;
var eth_deinit_calls: u32 = 0;
var attached_handler: EthHandler = null;
var attach_calls: u32 = 0;

export fn ra8_eth_init() callconv(.c) u16 {
    eth_init_calls += 1;
    return eth_init_rc;
}

export fn ra8_eth_deinit() callconv(.c) u16 {
    eth_deinit_calls += 1;
    return eth_deinit_rc;
}

export fn ra8_eth_attach_handler(handler: EthHandler, ctx: ?*anyopaque) callconv(.c) void {
    _ = ctx;
    attach_calls += 1;
    attached_handler = handler;
}

var error_calls: u32 = 0;
var info_calls: u32 = 0;
var error_val_calls: u32 = 0;
var last_error_message: [*:0]const u8 = "";
var last_error_value: u32 = 0;

export fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void {
    _ = tag;
    error_calls += 1;
    last_error_message = message;
}

export fn ra8_log_emit_info(tag: [*:0]const u8, message: [*:0]const u8) void {
    _ = tag;
    _ = message;
    info_calls += 1;
}

export fn ra8_log_emit_error_val(tag: [*:0]const u8, message: [*:0]const u8, value: u32) void {
    _ = tag;
    error_val_calls += 1;
    last_error_message = message;
    last_error_value = value;
}

var event_calls: u32 = 0;
var last_event_mask: u32 = 0;
var last_event_ctx: ?*anyopaque = null;

fn countingEvent(ctx: ?*anyopaque, mask: u32) callconv(.c) void {
    event_calls += 1;
    last_event_mask = mask;
    last_event_ctx = ctx;
}

const test_mac: abi.Mac = .{ .bytes = .{ 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 } };

/// Force the singleton back to its pre-init state, as the C suite's
/// `internal_prep` does, and clear every observation counter.
fn prep() void {
    eth_init_rc = ok;
    eth_deinit_rc = ok;
    _ = abi.ra8_net_pal_deinit();
    eth_init_calls = 0;
    eth_deinit_calls = 0;
    attach_calls = 0;
    attached_handler = null;
    error_calls = 0;
    info_calls = 0;
    error_val_calls = 0;
    last_error_value = 0;
    event_calls = 0;
    last_event_mask = 0;
    last_event_ctx = null;
}

fn messageEquals(expected: []const u8) bool {
    return std.mem.eql(u8, std.mem.span(last_error_message), expected);
}

test "init stores the supplied mac and starts the link down" {
    prep();
    try std.testing.expectEqual(ok, abi.ra8_net_pal_init(&test_mac));

    var got: abi.Mac = abi.Mac.zero;
    try std.testing.expectEqual(ok, abi.ra8_net_pal_get_mac_addr(&got));
    try std.testing.expectEqualSlices(u8, &test_mac.bytes, &got.bytes);

    var link: abi.LinkState = .up;
    try std.testing.expectEqual(ok, abi.ra8_net_pal_link_status(&link));
    try std.testing.expectEqual(abi.LinkState.down, link);
}

test "init with a null mac keeps the all-zero default" {
    prep();
    try std.testing.expectEqual(ok, abi.ra8_net_pal_init(null));
    var got: abi.Mac = test_mac;
    try std.testing.expectEqual(ok, abi.ra8_net_pal_get_mac_addr(&got));
    try std.testing.expectEqualSlices(u8, &abi.Mac.zero.bytes, &got.bytes);
}

test "init attaches the driver handler and logs the ready line" {
    prep();
    try std.testing.expectEqual(ok, abi.ra8_net_pal_init(&test_mac));
    try std.testing.expectEqual(@as(u32, 1), eth_init_calls);
    try std.testing.expectEqual(@as(u32, 1), attach_calls);
    try std.testing.expect(attached_handler != null);
    try std.testing.expectEqual(@as(u32, 1), info_calls);
}

test "init reports hw_init_failed with the driver code when the driver fails" {
    prep();
    eth_init_rc = 0x0207;
    try std.testing.expectEqual(hw_init_failed, abi.ra8_net_pal_init(&test_mac));
    try std.testing.expectEqual(@as(u32, 1), error_val_calls);
    try std.testing.expectEqual(@as(u32, 0x0207), last_error_value);
    try std.testing.expect(messageEquals("ra8_eth_init failed"));
}

test "a failed init leaves the PAL uninitialized and attaches nothing" {
    prep();
    eth_init_rc = 0x0207;
    try std.testing.expectEqual(hw_init_failed, abi.ra8_net_pal_init(null));
    try std.testing.expectEqual(@as(u32, 0), attach_calls);
    var got: abi.Mac = abi.Mac.zero;
    try std.testing.expectEqual(invalid_state, abi.ra8_net_pal_get_mac_addr(&got));
}

test "deinit before init reports invalid_state and does not call the driver" {
    prep();
    try std.testing.expectEqual(invalid_state, abi.ra8_net_pal_deinit());
    try std.testing.expectEqual(@as(u32, 0), eth_deinit_calls);
}

test "deinit detaches the driver handler" {
    prep();
    try std.testing.expectEqual(ok, abi.ra8_net_pal_init(&test_mac));
    try std.testing.expectEqual(ok, abi.ra8_net_pal_deinit());
    try std.testing.expectEqual(@as(u32, 1), eth_deinit_calls);
    try std.testing.expect(attached_handler == null);
}

test "deinit passes the driver's own error code back" {
    prep();
    try std.testing.expectEqual(ok, abi.ra8_net_pal_init(&test_mac));
    eth_deinit_rc = 0x0204;
    try std.testing.expectEqual(@as(u16, 0x0204), abi.ra8_net_pal_deinit());
    var link: abi.LinkState = .up;
    try std.testing.expectEqual(invalid_state, abi.ra8_net_pal_link_status(&link));
}

test "set_mac_addr rejects a null descriptor with its own log line" {
    prep();
    try std.testing.expectEqual(ok, abi.ra8_net_pal_init(&test_mac));
    try std.testing.expectEqual(null_ptr, abi.ra8_net_pal_set_mac_addr(null));
    try std.testing.expectEqual(@as(u32, 1), error_calls);
    try std.testing.expect(messageEquals("set_mac_addr: mac"));
}

test "set_mac_addr checks the null guard before the init guard" {
    prep();
    try std.testing.expectEqual(null_ptr, abi.ra8_net_pal_set_mac_addr(null));
}

test "set_mac_addr before init reports invalid_state" {
    prep();
    try std.testing.expectEqual(invalid_state, abi.ra8_net_pal_set_mac_addr(&test_mac));
}

test "set then get round-trips a new mac" {
    prep();
    try std.testing.expectEqual(ok, abi.ra8_net_pal_init(null));
    const other: abi.Mac = .{ .bytes = .{ 0x06, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE } };
    try std.testing.expectEqual(ok, abi.ra8_net_pal_set_mac_addr(&other));
    var got: abi.Mac = abi.Mac.zero;
    try std.testing.expectEqual(ok, abi.ra8_net_pal_get_mac_addr(&got));
    try std.testing.expectEqualSlices(u8, &other.bytes, &got.bytes);
}

test "get_mac_addr rejects a null output with its own log line" {
    prep();
    try std.testing.expectEqual(ok, abi.ra8_net_pal_init(&test_mac));
    try std.testing.expectEqual(null_ptr, abi.ra8_net_pal_get_mac_addr(null));
    try std.testing.expect(messageEquals("get_mac_addr: out_mac"));
}

test "get_mac_addr before init reports invalid_state" {
    prep();
    var got: abi.Mac = abi.Mac.zero;
    try std.testing.expectEqual(invalid_state, abi.ra8_net_pal_get_mac_addr(&got));
}

test "send_frame rejects a null frame with its own log line" {
    prep();
    try std.testing.expectEqual(ok, abi.ra8_net_pal_init(null));
    try std.testing.expectEqual(null_ptr, abi.ra8_net_pal_send_frame(null, 64));
    try std.testing.expect(messageEquals("send_frame: frame"));
}

test "send_frame checks the null guard before the init guard" {
    prep();
    try std.testing.expectEqual(null_ptr, abi.ra8_net_pal_send_frame(null, 64));
}

test "send_frame before init reports invalid_state" {
    prep();
    var buf = [_]u8{0} ** 64;
    try std.testing.expectEqual(invalid_state, abi.ra8_net_pal_send_frame(&buf, 64));
}

test "mcdc: send_frame length decision (len == 0 or len > frame_max)" {
    prep();
    try std.testing.expectEqual(ok, abi.ra8_net_pal_init(null));
    var buf = [_]u8{0} ** frame_max;
    // V1: both conditions false, the frame is accepted.
    try std.testing.expectEqual(ok, abi.ra8_net_pal_send_frame(&buf, 64));
    // V2: first condition true, short-circuits.
    try std.testing.expectEqual(invalid_arg, abi.ra8_net_pal_send_frame(&buf, 0));
    // V3: first false, second true.
    try std.testing.expectEqual(invalid_arg, abi.ra8_net_pal_send_frame(&buf, frame_max + 1));
    // The boundary itself stays valid.
    try std.testing.expectEqual(ok, abi.ra8_net_pal_send_frame(&buf, frame_max));
}

test "send_frame reports no_mem once the ring is full" {
    prep();
    try std.testing.expectEqual(ok, abi.ra8_net_pal_init(null));
    var buf = [_]u8{0x5A} ** 64;
    var i: u16 = 0;
    while (i < 4) : (i += 1) {
        try std.testing.expectEqual(ok, abi.ra8_net_pal_send_frame(&buf, 64));
    }
    try std.testing.expectEqual(no_mem, abi.ra8_net_pal_send_frame(&buf, 64));
}

test "recv_frame rejects a null buffer before a null length" {
    prep();
    try std.testing.expectEqual(ok, abi.ra8_net_pal_init(null));
    var len: u16 = frame_max;
    try std.testing.expectEqual(null_ptr, abi.ra8_net_pal_recv_frame(null, &len));
    try std.testing.expect(messageEquals("recv_frame: out_buf"));
}

test "recv_frame rejects a null length with its own log line" {
    prep();
    try std.testing.expectEqual(ok, abi.ra8_net_pal_init(null));
    var buf = [_]u8{0} ** frame_max;
    try std.testing.expectEqual(null_ptr, abi.ra8_net_pal_recv_frame(&buf, null));
    try std.testing.expect(messageEquals("recv_frame: inout_len"));
}

test "recv_frame before init reports invalid_state" {
    prep();
    var buf = [_]u8{0} ** frame_max;
    var len: u16 = frame_max;
    try std.testing.expectEqual(invalid_state, abi.ra8_net_pal_recv_frame(&buf, &len));
}

test "recv_frame demands full frame capacity" {
    prep();
    try std.testing.expectEqual(ok, abi.ra8_net_pal_init(null));
    var buf = [_]u8{0} ** frame_max;
    var short_len: u16 = 64;
    try std.testing.expectEqual(invalid_arg, abi.ra8_net_pal_recv_frame(&buf, &short_len));
    var edge_len: u16 = frame_max - 1;
    try std.testing.expectEqual(invalid_arg, abi.ra8_net_pal_recv_frame(&buf, &edge_len));
}

test "recv_frame on an empty ring reports no_data" {
    prep();
    try std.testing.expectEqual(ok, abi.ra8_net_pal_init(null));
    var buf = [_]u8{0} ** frame_max;
    var len: u16 = frame_max;
    try std.testing.expectEqual(no_data, abi.ra8_net_pal_recv_frame(&buf, &len));
}

test "send then recv loops a frame back with its length" {
    prep();
    try std.testing.expectEqual(ok, abi.ra8_net_pal_init(&test_mac));
    var frame: [64]u8 = undefined;
    for (&frame, 0..) |*byte, i| byte.* = @intCast(0xA0 +% i);
    try std.testing.expectEqual(ok, abi.ra8_net_pal_send_frame(&frame, frame.len));

    var buf = [_]u8{0} ** frame_max;
    var len: u16 = frame_max;
    try std.testing.expectEqual(ok, abi.ra8_net_pal_recv_frame(&buf, &len));
    try std.testing.expectEqual(@as(u16, 64), len);
    try std.testing.expectEqualSlices(u8, &frame, buf[0..64]);

    len = frame_max;
    try std.testing.expectEqual(no_data, abi.ra8_net_pal_recv_frame(&buf, &len));
}

test "init resets a ring left full by the previous session" {
    prep();
    try std.testing.expectEqual(ok, abi.ra8_net_pal_init(null));
    var buf = [_]u8{0x11} ** 64;
    var i: u16 = 0;
    while (i < 4) : (i += 1) {
        try std.testing.expectEqual(ok, abi.ra8_net_pal_send_frame(&buf, 64));
    }
    try std.testing.expectEqual(ok, abi.ra8_net_pal_init(null));
    var out = [_]u8{0} ** frame_max;
    var len: u16 = frame_max;
    try std.testing.expectEqual(no_data, abi.ra8_net_pal_recv_frame(&out, &len));
}

test "link_status rejects a null output with its own log line" {
    prep();
    try std.testing.expectEqual(ok, abi.ra8_net_pal_init(null));
    try std.testing.expectEqual(null_ptr, abi.ra8_net_pal_link_status(null));
    try std.testing.expect(messageEquals("link_status: out_state"));
}

test "link_status before init reports invalid_state" {
    prep();
    var link: abi.LinkState = .up;
    try std.testing.expectEqual(invalid_state, abi.ra8_net_pal_link_status(&link));
}

test "set_event_handler before init reports invalid_state" {
    prep();
    try std.testing.expectEqual(invalid_state, abi.ra8_net_pal_set_event_handler(countingEvent, null));
}

test "a successful send fans out tx_done to the attached handler" {
    prep();
    try std.testing.expectEqual(ok, abi.ra8_net_pal_init(null));
    var ctx_marker: u32 = 0xC0FFEE;
    try std.testing.expectEqual(ok, abi.ra8_net_pal_set_event_handler(countingEvent, &ctx_marker));
    var buf = [_]u8{0} ** 64;
    try std.testing.expectEqual(ok, abi.ra8_net_pal_send_frame(&buf, 64));
    try std.testing.expectEqual(@as(u32, 1), event_calls);
    try std.testing.expectEqual(@as(u32, 0x08), last_event_mask);
    try std.testing.expectEqual(@as(?*anyopaque, @ptrCast(&ctx_marker)), last_event_ctx);
}

test "detaching the handler stops the send fan-out" {
    prep();
    try std.testing.expectEqual(ok, abi.ra8_net_pal_init(null));
    try std.testing.expectEqual(ok, abi.ra8_net_pal_set_event_handler(countingEvent, null));
    try std.testing.expectEqual(ok, abi.ra8_net_pal_set_event_handler(null, null));
    event_calls = 0;
    var buf = [_]u8{0} ** 64;
    try std.testing.expectEqual(ok, abi.ra8_net_pal_send_frame(&buf, 64));
    try std.testing.expectEqual(@as(u32, 0), event_calls);
}

test "a failed send does not fan out an event" {
    prep();
    try std.testing.expectEqual(ok, abi.ra8_net_pal_init(null));
    try std.testing.expectEqual(ok, abi.ra8_net_pal_set_event_handler(countingEvent, null));
    var buf = [_]u8{0} ** 64;
    try std.testing.expectEqual(invalid_arg, abi.ra8_net_pal_send_frame(&buf, 0));
    try std.testing.expectEqual(@as(u32, 0), event_calls);
}

test "mcdc: driver dispatch guard, handler attached and status non-zero" {
    prep();
    try std.testing.expectEqual(ok, abi.ra8_net_pal_init(null));
    try std.testing.expectEqual(ok, abi.ra8_net_pal_set_event_handler(countingEvent, null));
    const handler = attached_handler orelse return error.TestUnexpectedResult;
    event_calls = 0;
    handler(null, 0x0000_0004);
    try std.testing.expectEqual(@as(u32, 1), event_calls);
    try std.testing.expectEqual(@as(u32, 0x10), last_event_mask);
}

test "mcdc: driver dispatch guard, handler attached and status clear" {
    prep();
    try std.testing.expectEqual(ok, abi.ra8_net_pal_init(null));
    try std.testing.expectEqual(ok, abi.ra8_net_pal_set_event_handler(countingEvent, null));
    const handler = attached_handler orelse return error.TestUnexpectedResult;
    event_calls = 0;
    handler(null, 0);
    try std.testing.expectEqual(@as(u32, 0), event_calls);
}

test "mcdc: driver dispatch guard, no handler and status non-zero" {
    prep();
    try std.testing.expectEqual(ok, abi.ra8_net_pal_init(null));
    const handler = attached_handler orelse return error.TestUnexpectedResult;
    event_calls = 0;
    handler(null, 0xFFFF_FFFF);
    try std.testing.expectEqual(@as(u32, 0), event_calls);
}

test "driver dispatch after deinit reaches nobody" {
    prep();
    try std.testing.expectEqual(ok, abi.ra8_net_pal_init(null));
    try std.testing.expectEqual(ok, abi.ra8_net_pal_set_event_handler(countingEvent, null));
    const handler = attached_handler orelse return error.TestUnexpectedResult;
    try std.testing.expectEqual(ok, abi.ra8_net_pal_deinit());
    event_calls = 0;
    handler(null, 0xFFFF_FFFF);
    try std.testing.expectEqual(@as(u32, 0), event_calls);
}

test "every pre-init entry point reports invalid_state" {
    prep();
    var mac: abi.Mac = abi.Mac.zero;
    var link: abi.LinkState = .up;
    var buf = [_]u8{0} ** frame_max;
    var len: u16 = frame_max;

    try std.testing.expectEqual(invalid_state, abi.ra8_net_pal_set_mac_addr(&test_mac));
    try std.testing.expectEqual(invalid_state, abi.ra8_net_pal_get_mac_addr(&mac));
    try std.testing.expectEqual(invalid_state, abi.ra8_net_pal_link_status(&link));
    try std.testing.expectEqual(invalid_state, abi.ra8_net_pal_send_frame(&buf, 64));
    try std.testing.expectEqual(invalid_state, abi.ra8_net_pal_recv_frame(&buf, &len));
    try std.testing.expectEqual(invalid_state, abi.ra8_net_pal_set_event_handler(countingEvent, null));
    try std.testing.expectEqual(invalid_state, abi.ra8_net_pal_deinit());
}
