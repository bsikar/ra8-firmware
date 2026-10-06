//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const sm = @import("smbus");

const Fake = struct {
    wrote: [sm.frame_bytes]u8 = undefined,
    wrote_len: usize = 0,
    wrote_addr: u8 = 0,
    wrote_stop: bool = false,
    cmd: ?u8 = null,
    rx: [sm.rx_bytes]u8 = @splat(0),
    rd_len: usize = 0,
    rd_addr: u8 = 0,
    ret: u16 = 0,
    errs: u8 = 0,

    pub fn write(f: *Fake, addr: u8, data: []const u8, stop: bool) u16 {
        f.wrote_addr = addr;
        f.wrote_stop = stop;
        f.wrote_len = data.len;
        @memcpy(f.wrote[0..data.len], data);
        return f.ret;
    }
    pub fn read(f: *Fake, addr: u8, data: []u8) u16 {
        f.rd_addr = addr;
        f.rd_len = data.len;
        @memcpy(data, f.rx[0..data.len]);
        return f.ret;
    }
    pub fn transfer(f: *Fake, addr: u8, wr: []const u8, rd: []u8) u16 {
        f.cmd = wr[0];
        return f.read(addr, rd);
    }
    pub fn logError(f: *Fake, _: [*:0]const u8) void {
        f.errs += 1;
    }
};

const Dev = sm.Smbus(*Fake);

fn ready(f: *Fake, use_pec: bool) Dev {
    var d: Dev = .{};
    _ = d.init(f, use_pec);
    return d;
}

test "pec matches the CRC-8 check value and an empty buffer gives zero" {
    try std.testing.expectEqual(@as(u8, 0xF4), sm.pec("123456789"));
    try std.testing.expectEqual(@as(u8, 0), sm.pec(""));
    try std.testing.expectEqual(@as(u8, 0xA1), sm.addrByte(0x50, 1));
}

test "every call before init reports not initialized" {
    var d: Dev = .{};
    var b: u8 = 0;
    var buf: [4]u8 = undefined;
    const ni = sm.not_initialized;
    try std.testing.expectEqual(ni, d.deinit());
    try std.testing.expectEqual(ni, d.sendByte(1, 2));
    try std.testing.expectEqual(ni, d.receiveByte(1, &b));
    try std.testing.expectEqual(ni, d.writeByteData(1, 2, 3));
    try std.testing.expectEqual(ni, d.readByteData(1, 2, &b));
    try std.testing.expectEqual(ni, d.blockWrite(1, 2, null, 0));
    try std.testing.expectEqual(ni, d.blockRead(1, 2, &buf, 0, &b));
    try std.testing.expectEqual(ni, d.alertRegister(null, null));
    try std.testing.expectEqual(ni, d.alertDispatch());
}

test "send byte and write byte data frame with and without PEC" {
    var f: Fake = .{};
    var d = ready(&f, false);
    try std.testing.expectEqual(sm.ok, d.sendByte(0x50, 0xAB));
    try std.testing.expectEqualSlices(u8, &.{0xAB}, f.wrote[0..f.wrote_len]);
    try std.testing.expect(f.wrote_stop);
    d = ready(&f, true);
    try std.testing.expectEqual(sm.ok, d.writeByteData(0x50, 0x10, 0x20));
    const want = sm.pec(&.{ 0xA0, 0x10, 0x20 });
    try std.testing.expectEqualSlices(u8, &.{ 0x10, 0x20, want }, f.wrote[0..f.wrote_len]);
    _ = d.sendByte(0x50, 0x7);
    try std.testing.expectEqualSlices(u8, &.{ 0x7, sm.pec(&.{ 0xA0, 0x7 }) }, f.wrote[0..f.wrote_len]);
}

test "receive byte checks PEC and leaves out alone on mismatch" {
    var f: Fake = .{};
    var d = ready(&f, true);
    var out: u8 = 0x55;
    f.rx[0] = 0x42;
    f.rx[1] = sm.pec(&.{ 0xA1, 0x42 });
    try std.testing.expectEqual(sm.ok, d.receiveByte(0x50, &out));
    try std.testing.expectEqual(@as(u8, 0x42), out);
    try std.testing.expectEqual(@as(usize, 2), f.rd_len);
    f.rx[1] ^= 1;
    out = 0x55;
    try std.testing.expectEqual(sm.crc_mismatch, d.receiveByte(0x50, &out));
    try std.testing.expectEqual(@as(u8, 0x55), out);
    try std.testing.expectEqual(@as(u8, 1), f.errs);
    f.ret = 0x203;
    try std.testing.expectEqual(@as(u16, 0x203), d.receiveByte(0x50, &out));
}

test "read byte data PEC covers write addr, cmd, read addr and data" {
    var f: Fake = .{};
    var d = ready(&f, true);
    var out: u8 = 0;
    f.rx[0] = 0x99;
    f.rx[1] = sm.pec(&.{ 0xA0, 0x0D, 0xA1, 0x99 });
    try std.testing.expectEqual(sm.ok, d.readByteData(0x50, 0x0D, &out));
    try std.testing.expectEqual(@as(u8, 0x99), out);
    try std.testing.expectEqual(@as(?u8, 0x0D), f.cmd);
    f.rx[1] ^= 0x80;
    try std.testing.expectEqual(sm.crc_mismatch, d.readByteData(0x50, 0x0D, &out));
    d = ready(&f, false);
    _ = d.readByteData(0x50, 0x0D, &out);
    try std.testing.expectEqual(@as(usize, 1), f.rd_len);
}

test "block write checks len before data and frames cmd, count, data, PEC" {
    var f: Fake = .{};
    var d = ready(&f, true);
    try std.testing.expectEqual(sm.invalid_arg, d.blockWrite(0x50, 1, null, 0));
    try std.testing.expectEqual(sm.null_ptr, d.blockWrite(0x50, 1, null, 2));
    const data = [_]u8{ 0xDE, 0xAD };
    try std.testing.expectEqual(sm.ok, d.blockWrite(0x50, 0x22, &data, 2));
    const want = sm.pec(&.{ 0xA0, 0x22, 2, 0xDE, 0xAD });
    try std.testing.expectEqualSlices(u8, &.{ 0x22, 2, 0xDE, 0xAD, want }, f.wrote[0..f.wrote_len]);
}

test "block write carries the full 255-byte payload" {
    var f: Fake = .{};
    var d = ready(&f, true);
    const data: [255]u8 = @splat(0x5A);
    try std.testing.expectEqual(sm.ok, d.blockWrite(0x50, 1, &data, 255));
    try std.testing.expectEqual(@as(usize, sm.frame_bytes), f.wrote_len);
}

test "block read sets out_len before the cap check" {
    var f: Fake = .{};
    var d = ready(&f, false);
    var buf: [2]u8 = .{ 0, 0 };
    var n: u8 = 0;
    try std.testing.expectEqual(sm.invalid_arg, d.blockRead(0x50, 1, &buf, 0, &n));
    f.rx[0] = 3;
    try std.testing.expectEqual(sm.invalid_size, d.blockRead(0x50, 1, &buf, 2, &n));
    try std.testing.expectEqual(@as(u8, 3), n);
    try std.testing.expectEqual(@as(usize, 3), f.rd_len);
}

test "block read copies data and checks PEC" {
    var f: Fake = .{};
    var d = ready(&f, true);
    var buf: [4]u8 = .{ 0, 0, 0, 0 };
    var n: u8 = 0;
    f.rx[0] = 2;
    f.rx[1] = 0x11;
    f.rx[2] = 0x22;
    f.rx[3] = sm.pec(&.{ 0xA0, 0x30, 0xA1, 2, 0x11, 0x22 });
    try std.testing.expectEqual(sm.ok, d.blockRead(0x50, 0x30, &buf, 4, &n));
    try std.testing.expectEqualSlices(u8, &.{ 0x11, 0x22 }, buf[0..n]);
    try std.testing.expectEqual(@as(usize, 6), f.rd_len);
    f.rx[3] ^= 1;
    try std.testing.expectEqual(sm.crc_mismatch, d.blockRead(0x50, 0x30, &buf, 4, &n));
}

var seen_addr: u8 = 0;
var seen_status: u8 = 0;
fn onAlert(_: ?*anyopaque, addr: u8, status: u8) callconv(.C) void {
    seen_addr = addr;
    seen_status = status;
}

test "alert dispatch reads the ARA and splits address and status" {
    var f: Fake = .{};
    var d = ready(&f, false);
    f.rx[0] = 0xA1;
    try std.testing.expectEqual(sm.ok, d.alertDispatch());
    try std.testing.expectEqual(sm.alert_addr_7b, f.rd_addr);
    try std.testing.expectEqual(sm.ok, d.alertRegister(&onAlert, null));
    try std.testing.expectEqual(sm.ok, d.alertDispatch());
    try std.testing.expectEqual(@as(u8, 0x50), seen_addr);
    try std.testing.expectEqual(@as(u8, 1), seen_status);
    f.ret = 0x203;
    try std.testing.expectEqual(@as(u16, 0x203), d.alertDispatch());
    try std.testing.expectEqual(sm.ok, d.deinit());
    try std.testing.expect(d.alert_fn == null);
    try std.testing.expectEqual(sm.not_initialized, d.deinit());
}
