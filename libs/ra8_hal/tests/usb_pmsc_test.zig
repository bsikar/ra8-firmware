//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for the USB device mass-storage BOT state machine (RA8FW-756).

const std = @import("std");
const pmsc = @import("usb_pmsc");

const Dev = struct {
    blocks: u32 = 64,
    reads: u32 = 0,

    fn of(ctx: ?*anyopaque) *Dev {
        return @ptrCast(@alignCast(ctx.?));
    }
    fn read(ctx: ?*anyopaque, _: u32, n: u32, buf: [*]u8) callconv(.C) u16 {
        of(ctx).reads += n;
        @memset(buf[0 .. n * 512], 0xA5);
        return 0;
    }
    fn write(_: ?*anyopaque, _: u32, _: u32, _: [*]const u8) callconv(.C) u16 {
        return 0;
    }
    fn capacity(ctx: ?*anyopaque, n: *u32, size: *u32) callconv(.C) u16 {
        n.* = of(ctx).blocks;
        size.* = 512;
        return 0;
    }
    fn inquiry(_: ?*anyopaque, v: [*]u8, _: [*]u8, _: [*]u8) callconv(.C) u16 {
        v[0] = 'R';
        return 0;
    }
    fn storage(self: *Dev) pmsc.Storage {
        return .{ .read_block = read, .write_block = write, .get_capacity = capacity, .get_inquiry = inquiry, .ctx = self };
    }
};

fn cbw(tag: u32, len: u32, dir_in: bool, cdb: []const u8) [31]u8 {
    var b = [_]u8{0} ** 31;
    std.mem.writeInt(u32, b[0..4], pmsc.cbw_signature, .little);
    std.mem.writeInt(u32, b[4..8], tag, .little);
    std.mem.writeInt(u32, b[8..12], len, .little);
    b[12] = if (dir_in) 0x80 else 0x00;
    b[13] = 0xF3; // LUN 3 plus reserved high bits
    b[14] = 0xEA; // CB length 10 plus reserved high bits
    @memcpy(b[15..][0..cdb.len], cdb);
    return b;
}

fn ready(dev: *Dev) pmsc.State {
    var st = std.mem.zeroes(pmsc.State);
    pmsc.resetForInit(&st, pmsc.speed_hs);
    const s = dev.storage();
    pmsc.attach(&st, &s);
    return st;
}

test "uninitialized or unattached state is rejected" {
    var st = std.mem.zeroes(pmsc.State);
    const b = cbw(1, 0, false, &.{0x00});
    try std.testing.expectEqual(pmsc.err_invalid_state, pmsc.feedCbw(&st, &b));
    try std.testing.expectEqual(pmsc.err_invalid_state, pmsc.step(&st));
    var out: [13]u8 = undefined;
    try std.testing.expectEqual(pmsc.err_invalid_state, pmsc.buildCsw(&st, 0, 0, &out));
    pmsc.resetForInit(&st, pmsc.speed_fs);
    try std.testing.expectEqual(pmsc.err_invalid_state, pmsc.feedCbw(&st, &b));
}

test "resetForInit clears everything but speed and flags" {
    var st = std.mem.zeroes(pmsc.State);
    st.cbw_tag = 9;
    st.storage_attached = true;
    st.cbw_cdb[3] = 7;
    pmsc.resetForInit(&st, pmsc.speed_hs);
    try std.testing.expect(st.initialized and !st.storage_attached);
    try std.testing.expectEqual(pmsc.speed_hs, st.speed);
    try std.testing.expectEqual(@as(u32, 0), st.cbw_tag);
    try std.testing.expectEqual(@as(u8, 0), st.cbw_cdb[3]);
    try std.testing.expect(st.storage.read_block == null);
}

test "feedCbw parses fields and masks LUN and CB length" {
    var dev = Dev{};
    var st = ready(&dev);
    const b = cbw(0x11223344, 512, true, &.{ 0x28, 0, 0, 0, 0, 1, 0, 0, 1, 0 });
    try std.testing.expectEqual(pmsc.ok, pmsc.feedCbw(&st, &b));
    try std.testing.expectEqual(@as(u32, 0x11223344), st.cbw_tag);
    try std.testing.expectEqual(@as(u32, 512), st.cbw_data_length);
    try std.testing.expect(st.cbw_dir_in);
    try std.testing.expectEqual(@as(u8, 3), st.cbw_lun);
    try std.testing.expectEqual(@as(u8, 10), st.cbw_cdb_len);
    try std.testing.expectEqual(@as(u8, 0x28), st.cbw_cdb[0]);
    try std.testing.expectEqual(pmsc.state_cdb_decode, st.bot_state);
}

test "bad signature latches the tag and moves to CSW" {
    var dev = Dev{};
    var st = ready(&dev);
    var b = cbw(0xCAFE, 0, false, &.{0x00});
    b[0] = 0;
    try std.testing.expectEqual(pmsc.err_invalid_arg, pmsc.feedCbw(&st, &b));
    try std.testing.expectEqual(@as(u32, 0xCAFE), st.cbw_tag);
    try std.testing.expectEqual(pmsc.state_csw_tx, st.bot_state);
}

test "dispatch preconditions" {
    var dev = Dev{};
    var st = ready(&dev);
    var buf: [512]u8 = undefined;
    var len: u32 = 99;
    var csw: u8 = 9;
    try std.testing.expectEqual(pmsc.err_invalid_state, pmsc.dispatch(&st, &buf, buf.len, &len, &csw));
    const b = cbw(1, 0, false, &.{0x00});
    _ = pmsc.feedCbw(&st, &b);
    try std.testing.expectEqual(pmsc.err_invalid_size, pmsc.dispatch(&st, &buf, 0, &len, &csw));
    try std.testing.expectEqual(@as(u32, 99), len);
}

test "TEST UNIT READY passes with no data and goes to CSW" {
    var dev = Dev{};
    var st = ready(&dev);
    const b = cbw(1, 0, false, &.{0x00});
    _ = pmsc.feedCbw(&st, &b);
    var buf: [64]u8 = undefined;
    var len: u32 = 5;
    var csw: u8 = 9;
    try std.testing.expectEqual(pmsc.ok, pmsc.dispatch(&st, &buf, buf.len, &len, &csw));
    try std.testing.expectEqual(@as(u32, 0), len);
    try std.testing.expectEqual(pmsc.csw_passed, csw);
    try std.testing.expectEqual(pmsc.state_csw_tx, st.bot_state);
}

test "READ(10) fills data and moves to data_tx" {
    var dev = Dev{};
    var st = ready(&dev);
    const b = cbw(2, 1024, true, &.{ 0x28, 0, 0, 0, 0, 4, 0, 0, 2, 0 });
    _ = pmsc.feedCbw(&st, &b);
    var buf: [1024]u8 = undefined;
    var len: u32 = 0;
    var csw: u8 = 9;
    try std.testing.expectEqual(pmsc.ok, pmsc.dispatch(&st, &buf, buf.len, &len, &csw));
    try std.testing.expectEqual(@as(u32, 1024), len);
    try std.testing.expectEqual(@as(u32, 2), dev.reads);
    try std.testing.expectEqual(pmsc.state_data_tx, st.bot_state);
    try std.testing.expectEqual(@as(u32, 1024), st.last_data_len);
}

test "INQUIRY into a short buffer fails the CSW with no data" {
    var dev = Dev{};
    var st = ready(&dev);
    const b = cbw(3, 36, true, &.{ 0x12, 0, 0, 0, 36, 0 });
    _ = pmsc.feedCbw(&st, &b);
    var buf: [8]u8 = undefined;
    var len: u32 = 0;
    var csw: u8 = 9;
    try std.testing.expectEqual(pmsc.ok, pmsc.dispatch(&st, &buf, buf.len, &len, &csw));
    try std.testing.expectEqual(pmsc.csw_failed, csw);
    try std.testing.expectEqual(@as(u32, 0), len);
    try std.testing.expectEqual(pmsc.state_csw_tx, st.bot_state);
}

test "unknown opcode fails the CSW" {
    var dev = Dev{};
    var st = ready(&dev);
    const b = cbw(4, 0, false, &.{0xFF});
    _ = pmsc.feedCbw(&st, &b);
    var buf: [16]u8 = undefined;
    var len: u32 = 0;
    var csw: u8 = 9;
    try std.testing.expectEqual(pmsc.ok, pmsc.dispatch(&st, &buf, buf.len, &len, &csw));
    try std.testing.expectEqual(pmsc.csw_failed, csw);
}

test "buildCsw packs signature, tag, residue and status" {
    var dev = Dev{};
    var st = ready(&dev);
    st.cbw_tag = 0xA1B2C3D4;
    st.bot_state = pmsc.state_csw_tx;
    var out = [_]u8{0xEE} ** 13;
    try std.testing.expectEqual(pmsc.ok, pmsc.buildCsw(&st, pmsc.csw_failed, 0x10, &out));
    try std.testing.expectEqualSlices(u8, &.{ 0x55, 0x53, 0x42, 0x53, 0xD4, 0xC3, 0xB2, 0xA1, 0x10, 0, 0, 0, 1 }, &out);
    try std.testing.expectEqual(pmsc.state_idle, st.bot_state);
}

test "step walks the BOT phases" {
    var dev = Dev{};
    var st = ready(&dev);
    const cases = [_][2]u8{
        .{ pmsc.state_idle, pmsc.state_cbw_rx },
        .{ pmsc.state_cbw_rx, pmsc.state_cbw_rx },
        .{ pmsc.state_cdb_decode, pmsc.state_cdb_decode },
        .{ pmsc.state_data_tx, pmsc.state_csw_tx },
        .{ pmsc.state_data_rx, pmsc.state_csw_tx },
        .{ pmsc.state_csw_tx, pmsc.state_idle },
        .{ 0x7F, pmsc.state_idle },
    };
    for (cases) |c| {
        st.bot_state = c[0];
        try std.testing.expectEqual(pmsc.ok, pmsc.step(&st));
        try std.testing.expectEqual(c[1], st.bot_state);
    }
}

test "bulk max packet and close" {
    try std.testing.expectEqual(@as(u16, 512), pmsc.bulkMaxPacket(pmsc.speed_hs));
    try std.testing.expectEqual(@as(u16, 64), pmsc.bulkMaxPacket(pmsc.speed_fs));
    var dev = Dev{};
    var st = ready(&dev);
    st.bot_state = pmsc.state_data_rx;
    pmsc.markClosed(&st);
    try std.testing.expect(!st.initialized and !st.storage_attached);
    try std.testing.expectEqual(pmsc.state_idle, st.bot_state);
}
