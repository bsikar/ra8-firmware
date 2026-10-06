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
//! `event_fn && pal_mask` can be varied here directly. The same substitution
//! stages a BMSR the host Ethernet fake cannot, so the link-up edge the
//! event-bit fix left host-unproven is covered here.

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

const EthHandler = fixture.EthHandler;

const fixture = @import("abi_fixture.zig");

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
    fixture.setEthInitResult(ok);
    fixture.setEthDeinitResult(ok);
    _ = abi.ra8_net_pal_deinit();
    fixture.reset();
    event_calls = 0;
    last_event_mask = 0;
    last_event_ctx = null;
}

fn messageEquals(expected: []const u8) bool {
    return std.mem.eql(u8, std.mem.span(fixture.logLastErrorMessage()), expected);
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
    try std.testing.expectEqual(@as(u32, 1), fixture.ethInitCalls());
    try std.testing.expectEqual(@as(u32, 1), fixture.ethAttachCalls());
    try std.testing.expect(fixture.attachedHandler() != null);
    try std.testing.expectEqual(@as(u32, 1), fixture.logInfoCalls());
}

test "init reports hw_init_failed with the driver code when the driver fails" {
    prep();
    fixture.setEthInitResult(0x0207);
    try std.testing.expectEqual(hw_init_failed, abi.ra8_net_pal_init(&test_mac));
    try std.testing.expectEqual(@as(u32, 1), fixture.logErrorValCalls());
    try std.testing.expectEqual(@as(u32, 0x0207), fixture.logLastErrorValue());
    try std.testing.expect(messageEquals("ra8_eth_init failed"));
}

test "a failed init leaves the PAL uninitialized and attaches nothing" {
    prep();
    fixture.setEthInitResult(0x0207);
    try std.testing.expectEqual(hw_init_failed, abi.ra8_net_pal_init(null));
    try std.testing.expectEqual(@as(u32, 0), fixture.ethAttachCalls());
    var got: abi.Mac = abi.Mac.zero;
    try std.testing.expectEqual(invalid_state, abi.ra8_net_pal_get_mac_addr(&got));
}

test "deinit before init reports invalid_state and does not call the driver" {
    prep();
    try std.testing.expectEqual(invalid_state, abi.ra8_net_pal_deinit());
    try std.testing.expectEqual(@as(u32, 0), fixture.ethDeinitCalls());
}

test "deinit detaches the driver handler" {
    prep();
    try std.testing.expectEqual(ok, abi.ra8_net_pal_init(&test_mac));
    try std.testing.expectEqual(ok, abi.ra8_net_pal_deinit());
    try std.testing.expectEqual(@as(u32, 1), fixture.ethDeinitCalls());
    try std.testing.expect(fixture.attachedHandler() == null);
}

test "deinit passes the driver's own error code back" {
    prep();
    try std.testing.expectEqual(ok, abi.ra8_net_pal_init(&test_mac));
    fixture.setEthDeinitResult(0x0204);
    try std.testing.expectEqual(@as(u16, 0x0204), abi.ra8_net_pal_deinit());
    var link: abi.LinkState = .up;
    try std.testing.expectEqual(invalid_state, abi.ra8_net_pal_link_status(&link));
}

test "set_mac_addr rejects a null descriptor with its own log line" {
    prep();
    try std.testing.expectEqual(ok, abi.ra8_net_pal_init(&test_mac));
    try std.testing.expectEqual(null_ptr, abi.ra8_net_pal_set_mac_addr(null));
    try std.testing.expectEqual(@as(u32, 1), fixture.logErrorCalls());
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
    var buf: [64]u8 = @splat(0);
    try std.testing.expectEqual(invalid_state, abi.ra8_net_pal_send_frame(&buf, 64));
}

test "mcdc: send_frame length decision (len == 0 or len > frame_max)" {
    prep();
    try std.testing.expectEqual(ok, abi.ra8_net_pal_init(null));
    var buf: [frame_max]u8 = @splat(0);
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
    var buf: [64]u8 = @splat(0x5A);
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
    var buf: [frame_max]u8 = @splat(0);
    try std.testing.expectEqual(null_ptr, abi.ra8_net_pal_recv_frame(&buf, null));
    try std.testing.expect(messageEquals("recv_frame: inout_len"));
}

test "recv_frame before init reports invalid_state" {
    prep();
    var buf: [frame_max]u8 = @splat(0);
    var len: u16 = frame_max;
    try std.testing.expectEqual(invalid_state, abi.ra8_net_pal_recv_frame(&buf, &len));
}

test "recv_frame demands full frame capacity" {
    prep();
    try std.testing.expectEqual(ok, abi.ra8_net_pal_init(null));
    var buf: [frame_max]u8 = @splat(0);
    var short_len: u16 = 64;
    try std.testing.expectEqual(invalid_arg, abi.ra8_net_pal_recv_frame(&buf, &short_len));
    var edge_len: u16 = frame_max - 1;
    try std.testing.expectEqual(invalid_arg, abi.ra8_net_pal_recv_frame(&buf, &edge_len));
}

test "recv_frame on an empty ring reports no_data" {
    prep();
    try std.testing.expectEqual(ok, abi.ra8_net_pal_init(null));
    var buf: [frame_max]u8 = @splat(0);
    var len: u16 = frame_max;
    try std.testing.expectEqual(no_data, abi.ra8_net_pal_recv_frame(&buf, &len));
}

test "send then recv loops a frame back with its length" {
    prep();
    try std.testing.expectEqual(ok, abi.ra8_net_pal_init(&test_mac));
    var frame: [64]u8 = undefined;
    for (&frame, 0..) |*byte, i| byte.* = @intCast(0xA0 +% i);
    try std.testing.expectEqual(ok, abi.ra8_net_pal_send_frame(&frame, frame.len));

    var buf: [frame_max]u8 = @splat(0);
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
    var buf: [64]u8 = @splat(0x11);
    var i: u16 = 0;
    while (i < 4) : (i += 1) {
        try std.testing.expectEqual(ok, abi.ra8_net_pal_send_frame(&buf, 64));
    }
    try std.testing.expectEqual(ok, abi.ra8_net_pal_init(null));
    var out: [frame_max]u8 = @splat(0);
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
    var buf: [64]u8 = @splat(0);
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
    var buf: [64]u8 = @splat(0);
    try std.testing.expectEqual(ok, abi.ra8_net_pal_send_frame(&buf, 64));
    try std.testing.expectEqual(@as(u32, 0), event_calls);
}

test "a failed send does not fan out an event" {
    prep();
    try std.testing.expectEqual(ok, abi.ra8_net_pal_init(null));
    try std.testing.expectEqual(ok, abi.ra8_net_pal_set_event_handler(countingEvent, null));
    var buf: [64]u8 = @splat(0);
    try std.testing.expectEqual(invalid_arg, abi.ra8_net_pal_send_frame(&buf, 0));
    try std.testing.expectEqual(@as(u32, 0), event_calls);
}

test "mcdc: driver dispatch guard, handler attached and status non-zero" {
    prep();
    try std.testing.expectEqual(ok, abi.ra8_net_pal_init(null));
    try std.testing.expectEqual(ok, abi.ra8_net_pal_set_event_handler(countingEvent, null));
    const handler = fixture.attachedHandler() orelse return error.TestUnexpectedResult;
    event_calls = 0;
    handler(null, 0x0000_0004);
    try std.testing.expectEqual(@as(u32, 1), event_calls);
    try std.testing.expectEqual(@as(u32, 0x10), last_event_mask);
}

test "mcdc: driver dispatch guard, handler attached and status clear" {
    prep();
    try std.testing.expectEqual(ok, abi.ra8_net_pal_init(null));
    try std.testing.expectEqual(ok, abi.ra8_net_pal_set_event_handler(countingEvent, null));
    const handler = fixture.attachedHandler() orelse return error.TestUnexpectedResult;
    event_calls = 0;
    handler(null, 0);
    try std.testing.expectEqual(@as(u32, 0), event_calls);
}

test "mcdc: driver dispatch guard, no handler and status non-zero" {
    prep();
    try std.testing.expectEqual(ok, abi.ra8_net_pal_init(null));
    const handler = fixture.attachedHandler() orelse return error.TestUnexpectedResult;
    event_calls = 0;
    handler(null, 0xFFFF_FFFF);
    try std.testing.expectEqual(@as(u32, 0), event_calls);
}

test "driver dispatch after deinit reaches nobody" {
    prep();
    try std.testing.expectEqual(ok, abi.ra8_net_pal_init(null));
    try std.testing.expectEqual(ok, abi.ra8_net_pal_set_event_handler(countingEvent, null));
    const handler = fixture.attachedHandler() orelse return error.TestUnexpectedResult;
    try std.testing.expectEqual(ok, abi.ra8_net_pal_deinit());
    event_calls = 0;
    handler(null, 0xFFFF_FFFF);
    try std.testing.expectEqual(@as(u32, 0), event_calls);
}

test "every pre-init entry point reports invalid_state" {
    prep();
    var mac: abi.Mac = abi.Mac.zero;
    var link: abi.LinkState = .up;
    var buf: [frame_max]u8 = @splat(0);
    var len: u16 = frame_max;

    try std.testing.expectEqual(invalid_state, abi.ra8_net_pal_set_mac_addr(&test_mac));
    try std.testing.expectEqual(invalid_state, abi.ra8_net_pal_get_mac_addr(&mac));
    try std.testing.expectEqual(invalid_state, abi.ra8_net_pal_link_status(&link));
    try std.testing.expectEqual(invalid_state, abi.ra8_net_pal_send_frame(&buf, 64));
    try std.testing.expectEqual(invalid_state, abi.ra8_net_pal_recv_frame(&buf, &len));
    try std.testing.expectEqual(invalid_state, abi.ra8_net_pal_set_event_handler(countingEvent, null));
    try std.testing.expectEqual(invalid_state, abi.ra8_net_pal_deinit());
}

test "dispatch with an empty ring reports the controller half alone" {
    prep();
    try std.testing.expectEqual(ok, abi.ra8_net_pal_init(&test_mac));
    try std.testing.expectEqual(ok, abi.ra8_net_pal_set_event_handler(countingEvent, null));
    const handler = fixture.attachedHandler() orelse return error.TestUnexpectedResult;

    event_calls = 0;
    handler(null, 0x0000_0002);
    try std.testing.expectEqual(@as(u32, 1), event_calls);
    try std.testing.expectEqual(@as(u32, 0x10), last_event_mask);
}

test "dispatch with a queued frame ORs rx_ready into the reported mask" {
    prep();
    try std.testing.expectEqual(ok, abi.ra8_net_pal_init(&test_mac));
    try std.testing.expectEqual(ok, abi.ra8_net_pal_set_event_handler(countingEvent, null));
    const handler = fixture.attachedHandler() orelse return error.TestUnexpectedResult;

    var frame: [64]u8 = @splat(0xA5);
    try std.testing.expectEqual(ok, abi.ra8_net_pal_send_frame(&frame, frame.len));

    event_calls = 0;
    handler(null, 0x0000_0002);
    try std.testing.expectEqual(@as(u32, 1), event_calls);
    try std.testing.expectEqual(@as(u32, 0x10 | 0x04), last_event_mask);
}

test "a queued frame alone is enough to dispatch on a clear status word" {
    prep();
    try std.testing.expectEqual(ok, abi.ra8_net_pal_init(&test_mac));
    try std.testing.expectEqual(ok, abi.ra8_net_pal_set_event_handler(countingEvent, null));
    const handler = fixture.attachedHandler() orelse return error.TestUnexpectedResult;

    var frame: [64]u8 = @splat(0x5A);
    try std.testing.expectEqual(ok, abi.ra8_net_pal_send_frame(&frame, frame.len));

    event_calls = 0;
    handler(null, 0);
    try std.testing.expectEqual(@as(u32, 1), event_calls);
    try std.testing.expectEqual(@as(u32, 0x04), last_event_mask);
}

test "draining the ring takes rx_ready back out of the dispatched mask" {
    prep();
    try std.testing.expectEqual(ok, abi.ra8_net_pal_init(&test_mac));
    try std.testing.expectEqual(ok, abi.ra8_net_pal_set_event_handler(countingEvent, null));
    const handler = fixture.attachedHandler() orelse return error.TestUnexpectedResult;

    var frame: [64]u8 = @splat(0x11);
    try std.testing.expectEqual(ok, abi.ra8_net_pal_send_frame(&frame, frame.len));
    var buf: [frame_max]u8 = @splat(0);
    var len: u16 = frame_max;
    try std.testing.expectEqual(ok, abi.ra8_net_pal_recv_frame(&buf, &len));

    event_calls = 0;
    handler(null, 0x0000_0002);
    try std.testing.expectEqual(@as(u32, 1), event_calls);
    try std.testing.expectEqual(@as(u32, 0x10), last_event_mask);
}

test "link_status with an unreadable PHY keeps the cached state and is silent" {
    prep();
    try std.testing.expectEqual(ok, abi.ra8_net_pal_init(&test_mac));
    try std.testing.expectEqual(ok, abi.ra8_net_pal_set_event_handler(countingEvent, null));

    event_calls = 0;
    var link: abi.LinkState = .up;
    try std.testing.expectEqual(ok, abi.ra8_net_pal_link_status(&link));
    try std.testing.expectEqual(abi.LinkState.down, link);
    try std.testing.expectEqual(@as(u32, 0), event_calls);
}

test "link_status with a readable PHY that agrees raises nothing" {
    prep();
    try std.testing.expectEqual(ok, abi.ra8_net_pal_init(&test_mac));
    try std.testing.expectEqual(ok, abi.ra8_net_pal_set_event_handler(countingEvent, null));
    fixture.setEthLinkResult(ok);
    fixture.setEthLinkUp(0);

    event_calls = 0;
    var link: abi.LinkState = .up;
    try std.testing.expectEqual(ok, abi.ra8_net_pal_link_status(&link));
    try std.testing.expectEqual(abi.LinkState.down, link);
    try std.testing.expectEqual(@as(u32, 0), event_calls);
}

test "link_status raises link_up once when the PHY comes up" {
    prep();
    try std.testing.expectEqual(ok, abi.ra8_net_pal_init(&test_mac));
    try std.testing.expectEqual(ok, abi.ra8_net_pal_set_event_handler(countingEvent, null));
    fixture.setEthLinkResult(ok);
    fixture.setEthLinkUp(1);

    event_calls = 0;
    var link: abi.LinkState = .down;
    try std.testing.expectEqual(ok, abi.ra8_net_pal_link_status(&link));
    try std.testing.expectEqual(abi.LinkState.up, link);
    try std.testing.expectEqual(@as(u32, 1), event_calls);
    try std.testing.expectEqual(@as(u32, 0x01), last_event_mask);

    // The edge is a transition, not a level: polling again is silent.
    event_calls = 0;
    try std.testing.expectEqual(ok, abi.ra8_net_pal_link_status(&link));
    try std.testing.expectEqual(abi.LinkState.up, link);
    try std.testing.expectEqual(@as(u32, 0), event_calls);
}

test "link_status raises link_down when the PHY drops again" {
    prep();
    try std.testing.expectEqual(ok, abi.ra8_net_pal_init(&test_mac));
    try std.testing.expectEqual(ok, abi.ra8_net_pal_set_event_handler(countingEvent, null));
    fixture.setEthLinkResult(ok);
    fixture.setEthLinkUp(1);
    var link: abi.LinkState = .down;
    try std.testing.expectEqual(ok, abi.ra8_net_pal_link_status(&link));

    fixture.setEthLinkUp(0);
    event_calls = 0;
    try std.testing.expectEqual(ok, abi.ra8_net_pal_link_status(&link));
    try std.testing.expectEqual(abi.LinkState.down, link);
    try std.testing.expectEqual(@as(u32, 1), event_calls);
    try std.testing.expectEqual(@as(u32, 0x02), last_event_mask);
}

test "a link edge with no handler attached updates the cache anyway" {
    prep();
    try std.testing.expectEqual(ok, abi.ra8_net_pal_init(&test_mac));
    fixture.setEthLinkResult(ok);
    fixture.setEthLinkUp(1);

    event_calls = 0;
    var link: abi.LinkState = .down;
    try std.testing.expectEqual(ok, abi.ra8_net_pal_link_status(&link));
    try std.testing.expectEqual(abi.LinkState.up, link);
    try std.testing.expectEqual(@as(u32, 0), event_calls);
}

test "link_status rejects a null output before it touches the PHY" {
    prep();
    try std.testing.expectEqual(ok, abi.ra8_net_pal_init(&test_mac));
    fixture.setEthLinkResult(ok);
    fixture.setEthLinkUp(1);

    const before = fixture.ethLinkCalls();
    try std.testing.expectEqual(null_ptr, abi.ra8_net_pal_link_status(null));
    try std.testing.expectEqual(before, fixture.ethLinkCalls());
}

test "a pre-init link_status never reads the PHY" {
    prep();
    fixture.setEthLinkResult(ok);
    fixture.setEthLinkUp(1);

    const before = fixture.ethLinkCalls();
    var link: abi.LinkState = .up;
    try std.testing.expectEqual(invalid_state, abi.ra8_net_pal_link_status(&link));
    try std.testing.expectEqual(before, fixture.ethLinkCalls());
}

test "deinit forgets a link the PHY had brought up" {
    prep();
    try std.testing.expectEqual(ok, abi.ra8_net_pal_init(&test_mac));
    fixture.setEthLinkResult(ok);
    fixture.setEthLinkUp(1);
    var link: abi.LinkState = .down;
    try std.testing.expectEqual(ok, abi.ra8_net_pal_link_status(&link));
    try std.testing.expectEqual(abi.LinkState.up, link);

    try std.testing.expectEqual(ok, abi.ra8_net_pal_deinit());
    try std.testing.expectEqual(ok, abi.ra8_net_pal_init(&test_mac));
    fixture.setEthLinkResult(0x010F);

    link = .up;
    try std.testing.expectEqual(ok, abi.ra8_net_pal_link_status(&link));
    try std.testing.expectEqual(abi.LinkState.down, link);
}
