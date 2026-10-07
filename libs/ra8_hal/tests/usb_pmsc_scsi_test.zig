//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/usb_pmsc_scsi.zig.

const std = @import("std");
const scsi = @import("usb_pmsc_scsi");

/// A fake block device: records the last read/write and can fail on demand.
const Dev = struct {
    count: u32 = 100,
    size: u32 = 512,
    fail: u16 = 0,
    lba: u32 = 0,
    blocks: u32 = 0,

    fn of(ctx: ?*anyopaque) *Dev {
        return @ptrCast(@alignCast(ctx.?));
    }
    fn read(ctx: ?*anyopaque, lba: u32, n: u32, buf: [*]u8) callconv(.c) u16 {
        const d = of(ctx);
        d.lba = lba;
        d.blocks = n;
        buf[0] = 0xAB;
        return d.fail;
    }
    fn write(ctx: ?*anyopaque, lba: u32, n: u32, _: [*]const u8) callconv(.c) u16 {
        const d = of(ctx);
        d.lba = lba;
        d.blocks = n;
        return d.fail;
    }
    fn capacity(ctx: ?*anyopaque, n: *u32, size: *u32) callconv(.c) u16 {
        const d = of(ctx);
        n.* = d.count;
        size.* = d.size;
        return d.fail;
    }
    fn inquiry(ctx: ?*anyopaque, v: [*]u8, p: [*]u8, r: [*]u8) callconv(.c) u16 {
        v[0] = 'R';
        p[0] = 'X';
        r[0] = '1';
        return of(ctx).fail;
    }
    fn storage(self: *Dev) scsi.Storage {
        return .{ .read_block = read, .write_block = write, .get_capacity = capacity, .get_inquiry = inquiry, .ctx = self };
    }
};

fn cdb10(lba: u32, n: u16) [16]u8 {
    var c: [16]u8 = @splat(0);
    std.mem.writeInt(u32, c[2..6], lba, .big);
    std.mem.writeInt(u16, c[7..9], n, .big);
    return c;
}

test "decodeRw10 reads the big-endian LBA and count" {
    const c = cdb10(0x0102_0304, 0x0506);
    const rw = scsi.decodeRw10(&c);
    try std.testing.expectEqual(@as(u32, 0x0102_0304), rw.lba);
    try std.testing.expectEqual(@as(u32, 0x0506), rw.count);
}

test "inquiry fills the header, pads with spaces and lets the backend write" {
    var d = Dev{};
    const s = d.storage();
    var buf: [40]u8 = @splat(0xEE);
    var n: u32 = 0;
    try std.testing.expectEqual(scsi.err_invalid_size, scsi.inquiry(&s, &buf, 35, &n));
    try std.testing.expectEqual(scsi.ok, scsi.inquiry(&s, &buf, 40, &n));
    try std.testing.expectEqual(@as(u32, 36), n);
    try std.testing.expectEqualSlices(u8, &.{ 0, 0x80, 0x04, 0x02, 0x1F, 0, 0, 0, 'R', ' ' }, buf[0..10]);
    try std.testing.expectEqual(@as(u8, 'X'), buf[16]);
    try std.testing.expectEqualSlices(u8, "1   ", buf[32..36]);
    try std.testing.expectEqual(@as(u8, 0xEE), buf[36]);
}

test "inquiry passes a backend error through without setting the length" {
    var d = Dev{ .fail = 0x201 };
    const s = d.storage();
    var buf: [36]u8 = @splat(0);
    var n: u32 = 7;
    try std.testing.expectEqual(@as(u16, 0x201), scsi.inquiry(&s, &buf, 36, &n));
    try std.testing.expectEqual(@as(u32, 7), n);
}

test "readCapacity packs last LBA and block size, clamping an empty device" {
    var d = Dev{ .count = 0x1_0000, .size = 512 };
    var s = d.storage();
    var buf: [8]u8 = @splat(0xEE);
    var n: u32 = 0;
    try std.testing.expectEqual(scsi.err_invalid_size, scsi.readCapacity(&s, &buf, 7, &n));
    try std.testing.expectEqual(scsi.ok, scsi.readCapacity(&s, &buf, 8, &n));
    try std.testing.expectEqualSlices(u8, &.{ 0, 0, 0xFF, 0xFF, 0, 0, 2, 0 }, &buf);
    try std.testing.expectEqual(@as(u32, 8), n);
    d.count = 0;
    s = d.storage();
    _ = scsi.readCapacity(&s, &buf, 8, &n);
    try std.testing.expectEqualSlices(u8, &.{ 0, 0, 0, 0 }, buf[0..4]);
}

test "requestSense and modeSense build fixed responses" {
    var buf: [18]u8 = @splat(0xEE);
    var n: u32 = 0;
    try std.testing.expectEqual(scsi.err_invalid_size, scsi.requestSense(&buf, 17, &n));
    try std.testing.expectEqual(scsi.ok, scsi.requestSense(&buf, 18, &n));
    try std.testing.expectEqual(@as(u32, 18), n);
    try std.testing.expectEqual(@as(u8, 0x70), buf[0]);
    try std.testing.expectEqual(@as(u8, 0x0A), buf[7]);
    try std.testing.expectEqual(@as(u8, 0), buf[17]);
    try std.testing.expectEqual(scsi.err_invalid_size, scsi.modeSense(&buf, 3, &n));
    try std.testing.expectEqual(scsi.ok, scsi.modeSense(&buf, 4, &n));
    try std.testing.expectEqualSlices(u8, &.{ 3, 0, 0, 0 }, buf[0..4]);
    try std.testing.expectEqual(@as(u32, 4), n);
}

test "read10 reads, checks the buffer size and defaults a zero block size" {
    var d = Dev{ .size = 0 };
    const s = d.storage();
    var buf: [1024]u8 = @splat(0);
    var n: u32 = 9;
    var c = cdb10(7, 0);
    try std.testing.expectEqual(scsi.ok, scsi.read10(&s, &c, &buf, 1024, &n));
    try std.testing.expectEqual(@as(u32, 0), n);
    c = cdb10(7, 3);
    try std.testing.expectEqual(scsi.err_invalid_size, scsi.read10(&s, &c, &buf, 1024, &n));
    c = cdb10(7, 2);
    try std.testing.expectEqual(scsi.ok, scsi.read10(&s, &c, &buf, 1024, &n));
    try std.testing.expectEqual(@as(u32, 1024), n);
    try std.testing.expectEqual(@as(u32, 7), d.lba);
    try std.testing.expectEqual(@as(u8, 0xAB), buf[0]);
}

test "write10 writes and reports the byte count, passing errors through" {
    var d = Dev{ .size = 4096 };
    const s = d.storage();
    const buf: [4]u8 = @splat(0);
    var n: u32 = 0;
    const c = cdb10(0x10, 4);
    try std.testing.expectEqual(scsi.ok, scsi.write10(&s, &c, &buf, &n));
    try std.testing.expectEqual(@as(u32, 16384), n);
    try std.testing.expectEqual(@as(u32, 4), d.blocks);
    d.fail = 0x203;
    n = 1;
    try std.testing.expectEqual(@as(u16, 0x203), scsi.write10(&s, &c, &buf, &n));
    try std.testing.expectEqual(@as(u32, 1), n);
}

test "State and Storage match the C layouts on host" {
    try std.testing.expectEqual(@as(usize, 80), @sizeOf(scsi.State));
    try std.testing.expectEqual(@as(usize, 8), @offsetOf(scsi.State, "storage"));
    try std.testing.expectEqual(@as(usize, 58), @offsetOf(scsi.State, "cbw_cdb"));
    try std.testing.expectEqual(@as(usize, 76), @offsetOf(scsi.State, "last_data_len"));
}
