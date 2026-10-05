//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! BLE HCI framing and dispatch (RA8FW-759).

const std = @import("std");
const ble = @import("ble");

const Rec = struct {
    events: usize = 0,
    acls: usize = 0,
    code: u8 = 0,
    handle: u16 = 0,
    last: [8]u8 = [_]u8{0} ** 8,
    len: usize = 0,
    pub fn event(self: *Rec, code: u8, params: []const u8) void {
        self.events += 1;
        self.code = code;
        self.keep(params);
    }
    pub fn acl(self: *Rec, handle: u16, payload: []const u8) void {
        self.acls += 1;
        self.handle = handle;
        self.keep(payload);
    }
    fn keep(self: *Rec, b: []const u8) void {
        self.len = b.len;
        @memcpy(self.last[0..@min(b.len, 8)], b[0..@min(b.len, 8)]);
    }
};

test "cfg flags: above 1 is invalid, set is not supported" {
    try std.testing.expectEqual(ble.Status.ok, ble.cfgCheck(.{ .use_external_osc = 0, .deep_sleep_enable = 0 }));
    try std.testing.expectEqual(ble.Status.invalid_arg, ble.cfgCheck(.{ .use_external_osc = 0, .deep_sleep_enable = 2 }));
    try std.testing.expectEqual(ble.Status.not_supported, ble.cfgCheck(.{ .use_external_osc = 1, .deep_sleep_enable = 0 }));
    try std.testing.expectEqual(ble.Status.not_supported, ble.cfgCheck(.{ .use_external_osc = 0, .deep_sleep_enable = 1 }));
}

test "command and ACL framing are little-endian with type bytes" {
    var s = ble.State{};
    s.sendCommand(0x200A, &.{1});
    s.sendAcl(0x0042, &.{ 0xAA, 0xBB });
    const want = [_]u8{ 0x01, 0x0A, 0x20, 0x01, 0x01, 0x02, 0x42, 0x00, 0x02, 0x00, 0xAA, 0xBB };
    try std.testing.expectEqualSlices(u8, &want, s.tx[0..s.tx_len]);
}

test "TX capture drops bytes past 1024" {
    var s = ble.State{};
    var i: usize = 0;
    while (i < ble.capture_bytes + 10) : (i += 1) s.txByte(@truncate(i));
    try std.testing.expectEqual(ble.capture_bytes, s.tx_len);
    try std.testing.expectEqual(@as(u8, 0xFF), s.tx[255]);
}

test "inject clamps, empty leaves the cursor, reset clears lengths" {
    var s = ble.State{};
    var big = [_]u8{7} ** (ble.capture_bytes + 4);
    s.inject(&big);
    try std.testing.expectEqual(ble.capture_bytes, s.rx_len);
    _ = s.rxByte();
    s.inject(&.{});
    try std.testing.expectEqual(@as(u16, 1), s.rx_pos);
    s.txByte(1);
    s.reset();
    try std.testing.expectEqual(@as(u16, 0), s.tx_len + s.rx_len + s.rx_pos);
}

test "scan window bounds and packet" {
    try std.testing.expect(ble.scanWindowOk(0x0010, 0x0010));
    try std.testing.expect(!ble.scanWindowOk(0x0003, 0x0003));
    try std.testing.expect(!ble.scanWindowOk(0x4001, 0x0010));
    try std.testing.expect(!ble.scanWindowOk(0x0010, 0x0020));
    const p = ble.scanParams(5, 0x1234, 0x0056);
    try std.testing.expectEqualSlices(u8, &.{ 1, 0x34, 0x12, 0x56, 0x00, 0, 0 }, &p);
}

test "dispatch delivers an event then an ACL packet" {
    var s = ble.State{};
    var r = Rec{};
    s.inject(&.{ 0x04, 0x0E, 0x02, 0x11, 0x22, 0x02, 0x01, 0x00, 0x01, 0x00, 0x99 });
    try std.testing.expectEqual(ble.Status.ok, s.dispatch(&r));
    try std.testing.expectEqual(@as(usize, 1), r.events);
    try std.testing.expectEqual(@as(u8, 0x0E), r.code);
    try std.testing.expectEqual(@as(usize, 1), r.acls);
    try std.testing.expectEqual(@as(u16, 1), r.handle);
    try std.testing.expectEqual(@as(u8, 0x99), r.last[0]);
}

test "dispatch rejects truncated, oversize and unknown packets" {
    var r = Rec{};
    var s = ble.State{};
    s.inject(&.{ 0x04, 0x0E, 0x03, 0x01 });
    try std.testing.expectEqual(ble.Status.invalid_arg, s.dispatch(&r));
    s.inject(&.{ 0x02, 0x01, 0x00, 0xFC, 0x00 });
    try std.testing.expectEqual(ble.Status.invalid_arg, s.dispatch(&r));
    s.inject(&.{0x09});
    try std.testing.expectEqual(ble.Status.invalid_arg, s.dispatch(&r));
    try std.testing.expectEqual(@as(usize, 0), r.events + r.acls);
}

test "dispatch stops after 64 packets" {
    var s = ble.State{};
    var r = Rec{};
    var buf: [65 * 3]u8 = undefined;
    for (0..65) |i| buf[i * 3 ..][0..3].* = .{ 0x04, 0x0F, 0x00 };
    s.inject(&buf);
    try std.testing.expectEqual(ble.Status.ok, s.dispatch(&r));
    try std.testing.expectEqual(@as(usize, 64), r.events);
}
