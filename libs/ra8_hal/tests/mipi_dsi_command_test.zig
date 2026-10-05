//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const cmd = @import("mipi_dsi_command");

const expectEqual = std.testing.expectEqual;

const Fake = struct {
    shorts: u8 = 0,
    longs: u8 = 0,
    errs: u8 = 0,
    dt: u8 = 0,
    vc: u8 = 0xFF,
    p0: u8 = 0xEE,
    p1: u8 = 0xEE,
    len: u16 = 0,
    low_power: bool = false,
    rc: u16 = 0,

    pub fn short(f: *Fake, dt: u8, vc: u8, p0: u8, p1: u8) u16 {
        f.shorts += 1;
        f.dt = dt;
        f.vc = vc;
        f.p0 = p0;
        f.p1 = p1;
        return f.rc;
    }
    pub fn long(f: *Fake, dt: u8, vc: u8, _: ?[*]const u8, len: u16, low_power: bool) u16 {
        f.longs += 1;
        f.dt = dt;
        f.vc = vc;
        f.len = len;
        f.low_power = low_power;
        return f.rc;
    }
    pub fn err(f: *Fake, _: [*:0]const u8) void {
        f.errs += 1;
    }
};

test "send_command_short forwards both params on VC0" {
    var f = Fake{};
    const params = [2]u8{ 0x29, 0x11 };
    try expectEqual(cmd.ok, cmd.sendShort(&f, 0x15, &params));
    try expectEqual(@as(u8, 1), f.shorts);
    try expectEqual(@as(u8, 0x15), f.dt);
    try expectEqual(cmd.vc0, f.vc);
    try expectEqual(@as(u8, 0x29), f.p0);
    try expectEqual(@as(u8, 0x11), f.p1);
}

test "send_command_short rejects null params with one log line" {
    var f = Fake{};
    try expectEqual(cmd.err_null_ptr, cmd.sendShort(&f, 0x15, null));
    try expectEqual(@as(u8, 1), f.errs);
    try expectEqual(@as(u8, 0), f.shorts);
}

test "send_command_long sends high-speed on VC0" {
    var f = Fake{};
    const data = [_]u8{ 1, 2, 3, 4 };
    try expectEqual(cmd.ok, cmd.sendLong(&f, 0x39, &data, 4));
    try expectEqual(@as(u8, 1), f.longs);
    try expectEqual(cmd.vc0, f.vc);
    try expectEqual(@as(u16, 4), f.len);
    try expectEqual(false, f.low_power);
}

test "send_command_long null payload: error when len > 0, allowed when empty" {
    var f = Fake{};
    try expectEqual(cmd.err_null_ptr, cmd.sendLong(&f, 0x39, null, 3));
    try expectEqual(@as(u8, 0), f.longs);
    try expectEqual(cmd.ok, cmd.sendLong(&f, 0x39, null, 0));
    try expectEqual(@as(u8, 1), f.longs);
    try expectEqual(@as(u8, 0), f.errs);
}

test "send_command_payload empty is a zero-padded short packet" {
    var f = Fake{};
    try expectEqual(cmd.ok, cmd.sendPayload(&f, 0x05, null, 0));
    try expectEqual(@as(u8, 1), f.shorts);
    try expectEqual(@as(u8, 0), f.p0);
    try expectEqual(@as(u8, 0), f.p1);
}

test "send_command_payload one byte pads the second param" {
    var f = Fake{};
    const data = [_]u8{0xAB};
    try expectEqual(cmd.ok, cmd.sendPayload(&f, 0x15, &data, 1));
    try expectEqual(@as(u8, 0xAB), f.p0);
    try expectEqual(@as(u8, 0), f.p1);
    try expectEqual(cmd.vc0, f.vc);
}

test "send_command_payload two bytes fill the short header" {
    var f = Fake{};
    const data = [_]u8{ 0xAB, 0xCD };
    try expectEqual(cmd.ok, cmd.sendPayload(&f, 0x15, &data, 2));
    try expectEqual(@as(u8, 1), f.shorts);
    try expectEqual(@as(u8, 0), f.longs);
    try expectEqual(@as(u8, 0xCD), f.p1);
}

test "send_command_payload three bytes go long via LP escape" {
    var f = Fake{};
    const data = [_]u8{ 1, 2, 3 };
    try expectEqual(cmd.ok, cmd.sendPayload(&f, 0x39, &data, 3));
    try expectEqual(@as(u8, 0), f.shorts);
    try expectEqual(@as(u8, 1), f.longs);
    try expectEqual(@as(u16, 3), f.len);
    try expectEqual(true, f.low_power);
}

test "send_command_payload null payload with len > 0 sends nothing" {
    var f = Fake{};
    try expectEqual(cmd.err_null_ptr, cmd.sendPayload(&f, 0x15, null, 1));
    try expectEqual(cmd.err_null_ptr, cmd.sendPayload(&f, 0x39, null, 8));
    try expectEqual(@as(u8, 0), f.shorts);
    try expectEqual(@as(u8, 0), f.longs);
}

test "sender status codes pass straight through" {
    var f = Fake{ .rc = 0x203 };
    const params = [2]u8{ 0, 0 };
    try expectEqual(@as(u16, 0x203), cmd.sendShort(&f, 0x05, &params));
    try expectEqual(@as(u16, 0x203), cmd.sendPayload(&f, 0x39, &[_]u8{ 1, 2, 3 }, 3));
}
