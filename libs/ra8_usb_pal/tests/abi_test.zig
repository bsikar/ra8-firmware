//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! ABI tests. This root exports its own `ra8_usb` device-mode fakes and log
//! sinks, so the membrane's externs bind here exactly as they bind against the
//! real Ring-3 driver in the firmware build. Guard order, error codes, and the
//! log message each guard emits are all asserted.

const std = @import("std");
const abi = @import("abi");

const err_ok: u16 = 0;
const err_no_mem: u16 = 0x102;
const err_invalid_arg: u16 = 0x103;
const err_invalid_state: u16 = 0x104;
const err_no_data: u16 = 0x10A;
const err_hw_init_failed: u16 = 0x201;
const err_null_ptr: u16 = 0x504;

const speed_fs: u8 = 0;
const speed_hs: u8 = 1;

// =============================================================================
// Log sink
// =============================================================================

var log_error_count: u32 = 0;
var log_info_count: u32 = 0;
var log_error_val_count: u32 = 0;
var last_error_message: [*:0]const u8 = "";
var last_error_value: u32 = 0;

export fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void {
    _ = tag;
    log_error_count += 1;
    last_error_message = message;
}

export fn ra8_log_emit_error_val(tag: [*:0]const u8, message: [*:0]const u8, value: u32) void {
    _ = tag;
    log_error_val_count += 1;
    last_error_message = message;
    last_error_value = value;
}

export fn ra8_log_emit_info(tag: [*:0]const u8, message: [*:0]const u8) void {
    _ = tag;
    _ = message;
    log_info_count += 1;
}

fn lastErrorIs(expected: []const u8) bool {
    return std.mem.eql(u8, std.mem.span(last_error_message), expected);
}

// =============================================================================
// ra8_usb device-mode fake
// =============================================================================

var usb_init_rc: u16 = err_ok;
var usb_deinit_rc: u16 = err_ok;
var usb_init_calls: u32 = 0;
var usb_deinit_calls: u32 = 0;
var usb_attach_calls: u32 = 0;
var usb_last_attached: bool = false;
var usb_handler: ?abi.UsbEventFn = null;
var usb_handler_speed: u8 = 0xFF;

export fn ra8_usb_device_init(speed: u8) u16 {
    _ = speed;
    usb_init_calls += 1;
    return usb_init_rc;
}

export fn ra8_usb_device_deinit(speed: u8) u16 {
    _ = speed;
    usb_deinit_calls += 1;
    return usb_deinit_rc;
}

export fn ra8_usb_device_attach(speed: u8, attached: bool) u16 {
    _ = speed;
    usb_attach_calls += 1;
    usb_last_attached = attached;
    return err_ok;
}

export fn ra8_usb_attach_handler(speed: u8, fn_ptr: ?abi.UsbEventFn, ctx: ?*anyopaque) void {
    _ = ctx;
    usb_handler_speed = speed;
    usb_handler = fn_ptr;
}

// =============================================================================
// PAL event sink
// =============================================================================

var pal_event_count: u32 = 0;
var pal_last_mask: u16 = 0;
var pal_last_speed: u8 = 0xFF;
var pal_last_ctx: ?*anyopaque = null;

fn palEvent(ctx: ?*anyopaque, speed: u8, event_mask: u16) callconv(.c) void {
    pal_event_count += 1;
    pal_last_ctx = ctx;
    pal_last_speed = speed;
    pal_last_mask = event_mask;
}

fn resetAll() void {
    abi.testResetState();
    log_error_count = 0;
    log_info_count = 0;
    log_error_val_count = 0;
    last_error_message = "";
    last_error_value = 0;
    usb_init_rc = err_ok;
    usb_deinit_rc = err_ok;
    usb_init_calls = 0;
    usb_deinit_calls = 0;
    usb_attach_calls = 0;
    usb_last_attached = false;
    usb_handler = null;
    usb_handler_speed = 0xFF;
    pal_event_count = 0;
    pal_last_mask = 0;
    pal_last_speed = 0xFF;
    pal_last_ctx = null;
}

fn bringUp() !void {
    resetAll();
    try std.testing.expectEqual(err_ok, abi.ra8_usb_pal_init(speed_fs));
}

fn openBulkIn(ep: u8, max_packet: u16) !void {
    try std.testing.expectEqual(err_ok, abi.ra8_usb_pal_ep_open(ep, 1, 2, max_packet));
}

// =============================================================================
// Promoted predicates through the exported symbols
// =============================================================================

test "priv should_dispatch_event: all three C MC/DC vectors" {
    var sink: u8 = 0;
    try std.testing.expect(!abi.priv_usb_pal_should_dispatch_event(null, 0x0001, 0x0000));
    try std.testing.expect(abi.priv_usb_pal_should_dispatch_event(&sink, 0x0001, 0x0000));
    try std.testing.expect(!abi.priv_usb_pal_should_dispatch_event(&sink, 0x0000, 0x0000));
}

test "priv ep_out_of_range: all three C MC/DC vectors" {
    try std.testing.expect(!abi.priv_usb_pal_ep_out_of_range(1, 10));
    try std.testing.expect(abi.priv_usb_pal_ep_out_of_range(0, 10));
    try std.testing.expect(abi.priv_usb_pal_ep_out_of_range(11, 10));
}

// =============================================================================
// init / deinit
// =============================================================================

test "init rejects a speed that is neither FS nor HS" {
    resetAll();
    try std.testing.expectEqual(err_invalid_arg, abi.ra8_usb_pal_init(2));
    try std.testing.expectEqual(@as(u32, 0), usb_init_calls);
}

test "init brings the driver up and installs the handler" {
    try bringUp();
    try std.testing.expectEqual(@as(u32, 1), usb_init_calls);
    try std.testing.expectEqual(speed_fs, usb_handler_speed);
    try std.testing.expect(usb_handler != null);
    try std.testing.expectEqual(@as(u32, 1), log_info_count);
}

test "init at high speed records the negotiated selector" {
    resetAll();
    try std.testing.expectEqual(err_ok, abi.ra8_usb_pal_init(speed_hs));
    try std.testing.expectEqual(speed_hs, usb_handler_speed);
}

test "a failing driver init reports hw_init_failed and logs the driver code" {
    resetAll();
    usb_init_rc = 0x204;
    try std.testing.expectEqual(err_hw_init_failed, abi.ra8_usb_pal_init(speed_fs));
    try std.testing.expectEqual(@as(u32, 1), log_error_val_count);
    try std.testing.expectEqual(@as(u32, 0x204), last_error_value);
    try std.testing.expect(lastErrorIs("ra8_usb_device_init failed"));
}

test "a failing driver init leaves the PAL uninitialized" {
    resetAll();
    usb_init_rc = 0x204;
    _ = abi.ra8_usb_pal_init(speed_fs);
    try std.testing.expectEqual(err_invalid_state, abi.ra8_usb_pal_attach(true));
    try std.testing.expect(usb_handler == null);
}

test "init starts detached with no event handler" {
    try bringUp();
    var state: u8 = 0xFF;
    try std.testing.expectEqual(err_ok, abi.ra8_usb_pal_get_state(&state));
    try std.testing.expectEqual(@as(u8, 0), state);
    try std.testing.expectEqual(@as(u32, 0), pal_event_count);
}

test "deinit before init is invalid_state" {
    resetAll();
    try std.testing.expectEqual(err_invalid_state, abi.ra8_usb_pal_deinit());
    try std.testing.expectEqual(@as(u32, 0), usb_deinit_calls);
}

test "deinit detaches, removes the handler and tears the driver down" {
    try bringUp();
    try std.testing.expectEqual(err_ok, abi.ra8_usb_pal_deinit());
    try std.testing.expect(!usb_last_attached);
    try std.testing.expect(usb_handler == null);
    try std.testing.expectEqual(@as(u32, 1), usb_deinit_calls);
}

test "deinit returns the driver's own code and still marks the PAL down" {
    try bringUp();
    usb_deinit_rc = 0x204;
    try std.testing.expectEqual(@as(u16, 0x204), abi.ra8_usb_pal_deinit());
    try std.testing.expectEqual(err_invalid_state, abi.ra8_usb_pal_attach(true));
}

test "deinit clears the installed PAL callback" {
    try bringUp();
    _ = abi.ra8_usb_pal_set_event_handler(palEvent, null);
    _ = abi.ra8_usb_pal_deinit();
    try std.testing.expectEqual(err_ok, abi.ra8_usb_pal_init(speed_fs));
    try openBulkIn(1, 64);
    try std.testing.expectEqual(err_ok, abi.ra8_usb_pal_ep_send(1, &[_]u8{7}, 1));
    try std.testing.expectEqual(@as(u32, 0), pal_event_count);
}

test "deinit empties every endpoint ring" {
    try bringUp();
    try openBulkIn(2, 64);
    try std.testing.expectEqual(err_ok, abi.ra8_usb_pal_ep_send(2, &[_]u8{1}, 1));
    _ = abi.ra8_usb_pal_deinit();
    try std.testing.expectEqual(err_ok, abi.ra8_usb_pal_init(speed_fs));
    try openBulkIn(2, 64);
    var buf: [4]u8 = undefined;
    var len: u16 = 4;
    try std.testing.expectEqual(err_no_data, abi.ra8_usb_pal_ep_recv(2, &buf, &len));
}

// =============================================================================
// attach / get_state
// =============================================================================

test "attach before init is invalid_state" {
    resetAll();
    try std.testing.expectEqual(err_invalid_state, abi.ra8_usb_pal_attach(true));
}

test "attach then detach tracks the cached state" {
    try bringUp();
    var state: u8 = 0xFF;
    try std.testing.expectEqual(err_ok, abi.ra8_usb_pal_attach(true));
    try std.testing.expectEqual(err_ok, abi.ra8_usb_pal_get_state(&state));
    try std.testing.expectEqual(@as(u8, 1), state);
    try std.testing.expectEqual(err_ok, abi.ra8_usb_pal_attach(false));
    try std.testing.expectEqual(err_ok, abi.ra8_usb_pal_get_state(&state));
    try std.testing.expectEqual(@as(u8, 0), state);
}

test "attach drives the D+ pull-up through the driver" {
    try bringUp();
    const before = usb_attach_calls;
    _ = abi.ra8_usb_pal_attach(true);
    try std.testing.expectEqual(before + 1, usb_attach_calls);
    try std.testing.expect(usb_last_attached);
}

test "get_state rejects a NULL out pointer before the init check" {
    resetAll();
    try std.testing.expectEqual(err_null_ptr, abi.ra8_usb_pal_get_state(null));
    try std.testing.expectEqual(@as(u32, 1), log_error_count);
    try std.testing.expect(lastErrorIs("get_state: out_state"));
}

test "get_state before init is invalid_state" {
    resetAll();
    var state: u8 = 0xFF;
    try std.testing.expectEqual(err_invalid_state, abi.ra8_usb_pal_get_state(&state));
    try std.testing.expectEqual(@as(u8, 0xFF), state);
}

// =============================================================================
// ep_open
// =============================================================================

test "ep_open before init is invalid_state" {
    resetAll();
    try std.testing.expectEqual(err_invalid_state, abi.ra8_usb_pal_ep_open(1, 1, 2, 64));
}

test "ep_open rejects endpoint zero and anything past the limit" {
    try bringUp();
    try std.testing.expectEqual(err_invalid_arg, abi.ra8_usb_pal_ep_open(0, 1, 2, 64));
    try std.testing.expectEqual(err_invalid_arg, abi.ra8_usb_pal_ep_open(11, 1, 2, 64));
}

test "ep_open accepts the descriptor-shaped address" {
    try bringUp();
    try std.testing.expectEqual(err_ok, abi.ra8_usb_pal_ep_open(0x83, 1, 2, 64));
    try std.testing.expectEqual(err_ok, abi.ra8_usb_pal_ep_send(3, &[_]u8{9}, 1));
}

test "ep_open rejects an out-of-range direction" {
    try bringUp();
    try std.testing.expectEqual(err_invalid_arg, abi.ra8_usb_pal_ep_open(1, 2, 2, 64));
}

test "ep_open rejects an out-of-range transfer type" {
    try bringUp();
    try std.testing.expectEqual(err_invalid_arg, abi.ra8_usb_pal_ep_open(1, 1, 4, 64));
}

test "ep_open rejects a zero or oversize max packet" {
    try bringUp();
    try std.testing.expectEqual(err_invalid_arg, abi.ra8_usb_pal_ep_open(1, 1, 2, 0));
    try std.testing.expectEqual(err_invalid_arg, abi.ra8_usb_pal_ep_open(1, 1, 2, 1025));
    try std.testing.expectEqual(err_ok, abi.ra8_usb_pal_ep_open(1, 1, 2, 1024));
}

test "ep_open guard order: the address is judged before the direction" {
    try bringUp();
    try std.testing.expectEqual(err_invalid_arg, abi.ra8_usb_pal_ep_open(0, 9, 9, 0));
}

test "re-opening an endpoint empties its ring" {
    try bringUp();
    try openBulkIn(4, 64);
    try std.testing.expectEqual(err_ok, abi.ra8_usb_pal_ep_send(4, &[_]u8{1}, 1));
    try openBulkIn(4, 64);
    var buf: [4]u8 = undefined;
    var len: u16 = 4;
    try std.testing.expectEqual(err_no_data, abi.ra8_usb_pal_ep_recv(4, &buf, &len));
}

test "every endpoint from 1 to the limit opens" {
    try bringUp();
    var ep: u8 = 1;
    while (ep <= 10) : (ep += 1) {
        try std.testing.expectEqual(err_ok, abi.ra8_usb_pal_ep_open(ep, 0, 3, 8));
    }
}

// =============================================================================
// ep_send
// =============================================================================

test "ep_send before init is invalid_state" {
    resetAll();
    try std.testing.expectEqual(err_invalid_state, abi.ra8_usb_pal_ep_send(1, &[_]u8{1}, 1));
}

test "ep_send rejects an out-of-range endpoint" {
    try bringUp();
    try std.testing.expectEqual(err_invalid_arg, abi.ra8_usb_pal_ep_send(0, &[_]u8{1}, 1));
    try std.testing.expectEqual(err_invalid_arg, abi.ra8_usb_pal_ep_send(11, &[_]u8{1}, 1));
}

test "ep_send reports null_ptr for a NULL buffer with a non-zero length" {
    try bringUp();
    try openBulkIn(1, 64);
    try std.testing.expectEqual(err_null_ptr, abi.ra8_usb_pal_ep_send(1, null, 4));
}

test "ep_send reports invalid_arg for an oversize length with a real buffer" {
    try bringUp();
    try openBulkIn(1, 64);
    try std.testing.expectEqual(err_invalid_arg, abi.ra8_usb_pal_ep_send(1, &[_]u8{1}, 1025));
}

test "ep_send reports null_ptr when both faults are present" {
    try bringUp();
    try openBulkIn(1, 64);
    try std.testing.expectEqual(err_null_ptr, abi.ra8_usb_pal_ep_send(1, null, 2000));
}

test "ep_send accepts a NULL buffer with a zero length" {
    try bringUp();
    try openBulkIn(1, 64);
    try std.testing.expectEqual(err_ok, abi.ra8_usb_pal_ep_send(1, null, 0));
}

test "ep_send on an unopened endpoint is invalid_state" {
    try bringUp();
    try std.testing.expectEqual(err_invalid_state, abi.ra8_usb_pal_ep_send(5, &[_]u8{1}, 1));
}

test "ep_send rejects a packet larger than the endpoint's max" {
    try bringUp();
    try openBulkIn(1, 8);
    const payload = [_]u8{0} ** 9;
    try std.testing.expectEqual(err_invalid_arg, abi.ra8_usb_pal_ep_send(1, &payload, 9));
}

test "ep_send fills the ring and then reports no_mem" {
    try bringUp();
    try openBulkIn(1, 64);
    var i: u8 = 0;
    while (i < 4) : (i += 1) {
        try std.testing.expectEqual(err_ok, abi.ra8_usb_pal_ep_send(1, &[_]u8{i}, 1));
    }
    try std.testing.expectEqual(err_no_mem, abi.ra8_usb_pal_ep_send(1, &[_]u8{9}, 1));
}

test "ep_send fires ep_in on the installed handler" {
    try bringUp();
    var ctx: u32 = 0xC0FFEE;
    _ = abi.ra8_usb_pal_set_event_handler(palEvent, &ctx);
    try openBulkIn(1, 64);
    try std.testing.expectEqual(err_ok, abi.ra8_usb_pal_ep_send(1, &[_]u8{1}, 1));
    try std.testing.expectEqual(@as(u32, 1), pal_event_count);
    try std.testing.expectEqual(@as(u16, 0x0010), pal_last_mask);
    try std.testing.expectEqual(speed_fs, pal_last_speed);
    try std.testing.expectEqual(@as(?*anyopaque, @ptrCast(&ctx)), pal_last_ctx);
}

test "a rejected ep_send fires no event" {
    try bringUp();
    _ = abi.ra8_usb_pal_set_event_handler(palEvent, null);
    try openBulkIn(1, 8);
    const payload = [_]u8{0} ** 9;
    _ = abi.ra8_usb_pal_ep_send(1, &payload, 9);
    try std.testing.expectEqual(@as(u32, 0), pal_event_count);
}

// =============================================================================
// ep_recv
// =============================================================================

test "ep_recv rejects a NULL out buffer first" {
    resetAll();
    var len: u16 = 4;
    try std.testing.expectEqual(err_null_ptr, abi.ra8_usb_pal_ep_recv(1, null, &len));
    try std.testing.expect(lastErrorIs("ep_recv: out_buf"));
}

test "ep_recv rejects a NULL length pointer second" {
    resetAll();
    var buf: [4]u8 = undefined;
    try std.testing.expectEqual(err_null_ptr, abi.ra8_usb_pal_ep_recv(1, &buf, null));
    try std.testing.expect(lastErrorIs("ep_recv: inout_len"));
}

test "ep_recv null guards run before the init check" {
    resetAll();
    try std.testing.expectEqual(err_null_ptr, abi.ra8_usb_pal_ep_recv(1, null, null));
    try std.testing.expect(lastErrorIs("ep_recv: out_buf"));
}

test "ep_recv before init is invalid_state" {
    resetAll();
    var buf: [4]u8 = undefined;
    var len: u16 = 4;
    try std.testing.expectEqual(err_invalid_state, abi.ra8_usb_pal_ep_recv(1, &buf, &len));
}

test "ep_recv rejects an out-of-range endpoint" {
    try bringUp();
    var buf: [4]u8 = undefined;
    var len: u16 = 4;
    try std.testing.expectEqual(err_invalid_arg, abi.ra8_usb_pal_ep_recv(0, &buf, &len));
    try std.testing.expectEqual(err_invalid_arg, abi.ra8_usb_pal_ep_recv(11, &buf, &len));
}

test "ep_recv rejects a zero capacity" {
    try bringUp();
    try openBulkIn(1, 64);
    var buf: [4]u8 = undefined;
    var len: u16 = 0;
    try std.testing.expectEqual(err_invalid_arg, abi.ra8_usb_pal_ep_recv(1, &buf, &len));
}

test "ep_recv on an unopened endpoint is invalid_state" {
    try bringUp();
    var buf: [4]u8 = undefined;
    var len: u16 = 4;
    try std.testing.expectEqual(err_invalid_state, abi.ra8_usb_pal_ep_recv(6, &buf, &len));
}

test "ep_recv on an empty ring is no_data" {
    try bringUp();
    try openBulkIn(1, 64);
    var buf: [4]u8 = undefined;
    var len: u16 = 4;
    try std.testing.expectEqual(err_no_data, abi.ra8_usb_pal_ep_recv(1, &buf, &len));
}

test "send then recv round-trips the payload and the length" {
    try bringUp();
    try openBulkIn(1, 64);
    try std.testing.expectEqual(err_ok, abi.ra8_usb_pal_ep_send(1, &[_]u8{ 1, 2, 3 }, 3));
    var buf: [8]u8 = undefined;
    var len: u16 = 8;
    try std.testing.expectEqual(err_ok, abi.ra8_usb_pal_ep_recv(1, &buf, &len));
    try std.testing.expectEqual(@as(u16, 3), len);
    try std.testing.expectEqualSlices(u8, &[_]u8{ 1, 2, 3 }, buf[0..3]);
}

test "ep_recv truncates to the caller's capacity" {
    try bringUp();
    try openBulkIn(1, 64);
    try std.testing.expectEqual(err_ok, abi.ra8_usb_pal_ep_send(1, &[_]u8{ 1, 2, 3, 4, 5 }, 5));
    var buf: [2]u8 = undefined;
    var len: u16 = 2;
    try std.testing.expectEqual(err_ok, abi.ra8_usb_pal_ep_recv(1, &buf, &len));
    try std.testing.expectEqual(@as(u16, 2), len);
    try std.testing.expectEqualSlices(u8, &[_]u8{ 1, 2 }, buf[0..]);
}

test "ep_recv is FIFO and drains the ring" {
    try bringUp();
    try openBulkIn(1, 64);
    try std.testing.expectEqual(err_ok, abi.ra8_usb_pal_ep_send(1, &[_]u8{0xA}, 1));
    try std.testing.expectEqual(err_ok, abi.ra8_usb_pal_ep_send(1, &[_]u8{0xB}, 1));
    var buf: [4]u8 = undefined;
    var len: u16 = 4;
    _ = abi.ra8_usb_pal_ep_recv(1, &buf, &len);
    try std.testing.expectEqual(@as(u8, 0xA), buf[0]);
    len = 4;
    _ = abi.ra8_usb_pal_ep_recv(1, &buf, &len);
    try std.testing.expectEqual(@as(u8, 0xB), buf[0]);
    len = 4;
    try std.testing.expectEqual(err_no_data, abi.ra8_usb_pal_ep_recv(1, &buf, &len));
}

test "draining the ring makes room for another send" {
    try bringUp();
    try openBulkIn(1, 64);
    var i: u8 = 0;
    while (i < 4) : (i += 1) _ = abi.ra8_usb_pal_ep_send(1, &[_]u8{i}, 1);
    var buf: [4]u8 = undefined;
    var len: u16 = 4;
    _ = abi.ra8_usb_pal_ep_recv(1, &buf, &len);
    try std.testing.expectEqual(err_ok, abi.ra8_usb_pal_ep_send(1, &[_]u8{0xEE}, 1));
}

test "a zero-length packet survives the round trip" {
    try bringUp();
    try openBulkIn(1, 64);
    try std.testing.expectEqual(err_ok, abi.ra8_usb_pal_ep_send(1, null, 0));
    var buf: [4]u8 = undefined;
    var len: u16 = 4;
    try std.testing.expectEqual(err_ok, abi.ra8_usb_pal_ep_recv(1, &buf, &len));
    try std.testing.expectEqual(@as(u16, 0), len);
}

test "ep_recv accepts the descriptor-shaped address" {
    try bringUp();
    try openBulkIn(7, 64);
    try std.testing.expectEqual(err_ok, abi.ra8_usb_pal_ep_send(0x87, &[_]u8{0x5A}, 1));
    var buf: [4]u8 = undefined;
    var len: u16 = 4;
    try std.testing.expectEqual(err_ok, abi.ra8_usb_pal_ep_recv(0x87, &buf, &len));
    try std.testing.expectEqual(@as(u8, 0x5A), buf[0]);
}

test "endpoints do not share a ring" {
    try bringUp();
    try openBulkIn(1, 64);
    try openBulkIn(2, 64);
    try std.testing.expectEqual(err_ok, abi.ra8_usb_pal_ep_send(1, &[_]u8{1}, 1));
    var buf: [4]u8 = undefined;
    var len: u16 = 4;
    try std.testing.expectEqual(err_no_data, abi.ra8_usb_pal_ep_recv(2, &buf, &len));
}

// =============================================================================
// set_event_handler and the installed ra8_usb handler
// =============================================================================

test "set_event_handler before init is invalid_state" {
    resetAll();
    try std.testing.expectEqual(err_invalid_state, abi.ra8_usb_pal_set_event_handler(palEvent, null));
}

test "set_event_handler accepts NULL as a detach and logs nothing" {
    try bringUp();
    _ = abi.ra8_usb_pal_set_event_handler(palEvent, null);
    try std.testing.expectEqual(err_ok, abi.ra8_usb_pal_set_event_handler(null, null));
    try std.testing.expectEqual(@as(u32, 0), log_error_count);
    try openBulkIn(1, 64);
    _ = abi.ra8_usb_pal_ep_send(1, &[_]u8{1}, 1);
    try std.testing.expectEqual(@as(u32, 0), pal_event_count);
}

test "the installed usb handler forwards a raised status as an error event" {
    try bringUp();
    _ = abi.ra8_usb_pal_set_event_handler(palEvent, null);
    usb_handler.?(null, speed_fs, 0x0004);
    try std.testing.expectEqual(@as(u32, 1), pal_event_count);
    try std.testing.expectEqual(@as(u16, 0x8000), pal_last_mask);
}

test "the usb handler drops a clear status (dispatch MC/DC condition 2)" {
    try bringUp();
    _ = abi.ra8_usb_pal_set_event_handler(palEvent, null);
    usb_handler.?(null, speed_fs, 0x0000);
    try std.testing.expectEqual(@as(u32, 0), pal_event_count);
}

test "the usb handler drops events with no callback installed (condition 1)" {
    try bringUp();
    usb_handler.?(null, speed_fs, 0x0004);
    try std.testing.expectEqual(@as(u32, 0), pal_event_count);
}

test "the usb handler drops events from the other controller" {
    try bringUp();
    _ = abi.ra8_usb_pal_set_event_handler(palEvent, null);
    abi.testUsbEventHandler()(null, speed_hs, 0x0004);
    try std.testing.expectEqual(@as(u32, 0), pal_event_count);
}

test "the usb handler drops events once the PAL is down" {
    try bringUp();
    _ = abi.ra8_usb_pal_set_event_handler(palEvent, null);
    const handler = abi.testUsbEventHandler();
    _ = abi.ra8_usb_pal_deinit();
    handler(null, speed_fs, 0x0004);
    try std.testing.expectEqual(@as(u32, 0), pal_event_count);
}

test "the usb handler hands the stored context back" {
    try bringUp();
    var ctx: u32 = 42;
    _ = abi.ra8_usb_pal_set_event_handler(palEvent, &ctx);
    usb_handler.?(null, speed_fs, 0x0001);
    try std.testing.expectEqual(@as(?*anyopaque, @ptrCast(&ctx)), pal_last_ctx);
}

test "a full send/recv session over the high-speed controller" {
    resetAll();
    try std.testing.expectEqual(err_ok, abi.ra8_usb_pal_init(speed_hs));
    try std.testing.expectEqual(err_ok, abi.ra8_usb_pal_attach(true));
    try std.testing.expectEqual(err_ok, abi.ra8_usb_pal_ep_open(0x82, 1, 2, 512));
    const payload = [_]u8{0x5A} ** 512;
    try std.testing.expectEqual(err_ok, abi.ra8_usb_pal_ep_send(2, &payload, 512));
    var buf: [512]u8 = undefined;
    var len: u16 = 512;
    try std.testing.expectEqual(err_ok, abi.ra8_usb_pal_ep_recv(2, &buf, &len));
    try std.testing.expectEqual(@as(u16, 512), len);
    try std.testing.expectEqualSlices(u8, payload[0..], buf[0..]);
    try std.testing.expectEqual(err_ok, abi.ra8_usb_pal_deinit());
}
