//! Tests for the ra8_ftl C ABI membrane, driven over a fake underlying
//! erase-before-write block device. The fake counts erases, programs and
//! syncs, and can be told to fail a specific operation, so the guard order,
//! the copy-on-write relocation and the error propagation are all exercised
//! host-side with no hardware and no C archive.

const std = @import("std");
const abi = @import("abi");
const core = abi.core;

const expect = std.testing.expect;
const expectEqual = std.testing.expectEqual;

const block = core.block_size_bytes;

/// The log sink the archive calls: counts lines and records the last one.
var log_count: usize = 0;
var last_message: []const u8 = "";

export fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void {
    _ = tag;
    log_count += 1;
    last_message = std.mem.span(message);
}

fn resetLog() void {
    log_count = 0;
    last_message = "";
}

/// A fake erase-before-write device over an in-memory medium.
const Fake = struct {
    const total_blocks: u32 = 6;

    medium: [total_blocks * block]u8 = undefined,
    caps: core.Caps = .{
        .block_count = total_blocks,
        .erase_unit_blocks = 1,
        .program_size_bytes = block,
        .logical_block_bytes = @intCast(block),
        .erase_value = 0xFF,
        .must_erase_before_write = true,
        .read_only = false,
    },
    erases: u32 = 0,
    programs: u32 = 0,
    reads: u32 = 0,
    syncs: u32 = 0,
    caps_status: core.Err = core.ok,
    erase_status: core.Err = core.ok,
    write_status: core.Err = core.ok,
    read_status: core.Err = core.ok,
    sync_status: core.Err = core.ok,
    /// Let the first N erases succeed, then fail. -1 disables the knob.
    fail_erase_after: i32 = -1,

    fn init(self: *Fake) void {
        @memset(&self.medium, 0xFF);
    }

    fn devRead(ctx: ?*anyopaque, lba: u32, count: u32, buf: [*]u8) callconv(.c) core.Err {
        const self: *Fake = @ptrCast(@alignCast(ctx.?));
        self.reads += 1;
        if (self.read_status != core.ok) return self.read_status;
        const off = @as(usize, lba) * block;
        @memcpy(buf[0 .. count * block], self.medium[off .. off + count * block]);
        return core.ok;
    }

    fn devWrite(ctx: ?*anyopaque, lba: u32, count: u32, buf: [*]const u8) callconv(.c) core.Err {
        const self: *Fake = @ptrCast(@alignCast(ctx.?));
        self.programs += 1;
        if (self.write_status != core.ok) return self.write_status;
        const off = @as(usize, lba) * block;
        @memcpy(self.medium[off .. off + count * block], buf[0 .. count * block]);
        return core.ok;
    }

    fn devErase(ctx: ?*anyopaque, lba: u32, count: u32) callconv(.c) core.Err {
        const self: *Fake = @ptrCast(@alignCast(ctx.?));
        if (self.fail_erase_after >= 0 and self.erases >= @as(u32, @intCast(self.fail_erase_after))) {
            self.erases += 1;
            return 0x301;
        }
        self.erases += 1;
        if (self.erase_status != core.ok) return self.erase_status;
        const off = @as(usize, lba) * block;
        @memset(self.medium[off .. off + count * block], self.caps.erase_value);
        return core.ok;
    }

    fn devGetCaps(ctx: ?*const anyopaque, out: *core.Caps) callconv(.c) core.Err {
        const self: *const Fake = @ptrCast(@alignCast(ctx.?));
        if (self.caps_status != core.ok) return self.caps_status;
        out.* = self.caps;
        return core.ok;
    }

    fn devSync(ctx: ?*anyopaque) callconv(.c) core.Err {
        const self: *Fake = @ptrCast(@alignCast(ctx.?));
        self.syncs += 1;
        return self.sync_status;
    }

    const iface: abi.BlockdevIface = .{
        .read = devRead,
        .write = devWrite,
        .erase = devErase,
        .get_caps = devGetCaps,
        .sync = devSync,
    };
};

/// One fully wired FTL over a fake device: 4 logical over 6 physical.
const Rig = struct {
    fake: Fake = .{},
    raw: abi.Blockdev = .{},
    ftl: abi.Ftl = .{},
    map: [4]u16 = undefined,
    pblocks: [6]core.Pblock = undefined,
    scratch: [block]u8 = undefined,
    bd: abi.Blockdev = .{},

    fn init(self: *Rig) core.Err {
        self.fake = .{};
        self.fake.init();
        self.raw = .{ .iface = &Fake.iface, .ctx = &self.fake };
        self.map = undefined;
        self.pblocks = [_]core.Pblock{.{}} ** 6;
        self.ftl = .{};
        return abi.ra8_ftl_init(&self.ftl, &self.raw, &self.map, 4, &self.pblocks, 6, &self.scratch);
    }

    fn bind(self: *Rig) core.Err {
        return abi.ra8_ftl_as_blockdev(&self.ftl, &self.bd);
    }
};

// The archive's own front door into the underlying device. The production
// build links ra8_io's C implementation; here the test supplies the same
// five symbols so the membrane is exercised end to end.
export fn ra8_io_blockdev_read(bd: *const abi.Blockdev, lba: u32, count: u32, buf: [*]u8) core.Err {
    return bd.iface.?.read.?(bd.ctx, lba, count, buf);
}
export fn ra8_io_blockdev_write(bd: *const abi.Blockdev, lba: u32, count: u32, buf: [*]const u8) core.Err {
    return bd.iface.?.write.?(bd.ctx, lba, count, buf);
}
export fn ra8_io_blockdev_erase(bd: *const abi.Blockdev, lba: u32, count: u32) core.Err {
    return bd.iface.?.erase.?(bd.ctx, lba, count);
}
export fn ra8_io_blockdev_sync(bd: *const abi.Blockdev) core.Err {
    return bd.iface.?.sync.?(bd.ctx);
}
export fn ra8_io_blockdev_get_caps(bd: *const abi.Blockdev, out: *core.Caps) core.Err {
    return bd.iface.?.get_caps.?(bd.ctx, out);
}

test "layout: the handle matches the C ABI" {
    const ptr = @sizeOf(usize);
    try expectEqual(ptr * 4, @offsetOf(abi.Ftl, "logical_blocks"));
    try expectEqual(ptr * 4 + 4, @offsetOf(abi.Ftl, "physical_blocks"));
    try expectEqual(ptr * 4 + 8, @offsetOf(abi.Ftl, "erase_value"));
    try expectEqual(ptr * 2, @sizeOf(abi.Blockdev));
}

test "init: every NULL argument answers null_ptr with its own log line" {
    var rig: Rig = .{};
    rig.fake.init();
    rig.raw = .{ .iface = &Fake.iface, .ctx = &rig.fake };

    resetLog();
    try expectEqual(core.err_null_ptr, abi.ra8_ftl_init(null, &rig.raw, &rig.map, 4, &rig.pblocks, 6, &rig.scratch));
    try std.testing.expectEqualStrings("bd must not be nullptr", last_message);

    try expectEqual(core.err_null_ptr, abi.ra8_ftl_init(&rig.ftl, null, &rig.map, 4, &rig.pblocks, 6, &rig.scratch));
    try std.testing.expectEqualStrings("raw must not be nullptr", last_message);

    try expectEqual(core.err_null_ptr, abi.ra8_ftl_init(&rig.ftl, &rig.raw, null, 4, &rig.pblocks, 6, &rig.scratch));
    try std.testing.expectEqualStrings("map must not be nullptr", last_message);

    try expectEqual(core.err_null_ptr, abi.ra8_ftl_init(&rig.ftl, &rig.raw, &rig.map, 4, null, 6, &rig.scratch));
    try std.testing.expectEqualStrings("pblocks must not be nullptr", last_message);

    try expectEqual(core.err_null_ptr, abi.ra8_ftl_init(&rig.ftl, &rig.raw, &rig.map, 4, &rig.pblocks, 6, null));
    try std.testing.expectEqualStrings("scratch must not be nullptr", last_message);

    try expectEqual(@as(usize, 5), log_count);
}

test "init: the sizing rejections are judged after the NULL guards" {
    var rig: Rig = .{};
    rig.fake.init();
    rig.raw = .{ .iface = &Fake.iface, .ctx = &rig.fake };
    try expectEqual(core.err_invalid_size, abi.ra8_ftl_init(&rig.ftl, &rig.raw, &rig.map, 0, &rig.pblocks, 6, &rig.scratch));
    try expectEqual(core.err_invalid_size, abi.ra8_ftl_init(&rig.ftl, &rig.raw, &rig.map, 4, &rig.pblocks, core.max_pblocks + 1, &rig.scratch));
}

test "init: a failing caps query propagates before any table is touched" {
    var rig: Rig = .{};
    rig.fake.init();
    rig.fake.caps_status = 0x201;
    rig.raw = .{ .iface = &Fake.iface, .ctx = &rig.fake };
    rig.pblocks = [_]core.Pblock{.{}} ** 6;
    try expectEqual(@as(core.Err, 0x201), abi.ra8_ftl_init(&rig.ftl, &rig.raw, &rig.map, 4, &rig.pblocks, 6, &rig.scratch));
    try expect(rig.ftl.raw == null);
}

test "init: an unsuitable device is invalid_arg and leaves the handle unbound" {
    var rig: Rig = .{};
    rig.fake.init();
    rig.fake.caps.read_only = true;
    rig.raw = .{ .iface = &Fake.iface, .ctx = &rig.fake };
    try expectEqual(core.err_invalid_arg, abi.ra8_ftl_init(&rig.ftl, &rig.raw, &rig.map, 4, &rig.pblocks, 6, &rig.scratch));
    try expect(rig.ftl.raw == null);
}

test "init: success records the geometry and cold-starts both tables" {
    var rig: Rig = .{};
    try expectEqual(core.ok, rig.init());
    try expectEqual(@as(u32, 4), rig.ftl.logical_blocks);
    try expectEqual(@as(u32, 6), rig.ftl.physical_blocks);
    try expectEqual(@as(u8, 0xFF), rig.ftl.erase_value);
    for (rig.map) |entry| try expectEqual(core.unmapped, entry);
    for (rig.pblocks) |pb| try expectEqual(core.pstate_free, pb.state);
    try expectEqual(@as(u32, 0), rig.fake.erases);
}

test "as_blockdev: NULL arguments and an uninitialised handle" {
    var rig: Rig = .{};
    resetLog();
    try expectEqual(core.err_null_ptr, abi.ra8_ftl_as_blockdev(null, &rig.bd));
    try std.testing.expectEqualStrings("ftl must not be nullptr", last_message);
    try expectEqual(core.err_null_ptr, abi.ra8_ftl_as_blockdev(&rig.ftl, null));
    try std.testing.expectEqualStrings("out must not be nullptr", last_message);
    try expectEqual(core.err_not_initialized, abi.ra8_ftl_as_blockdev(&rig.ftl, &rig.bd));
    try expect(rig.bd.iface == null);
}

test "as_blockdev: binds the presented vtable with the handle as context" {
    var rig: Rig = .{};
    try expectEqual(core.ok, rig.init());
    try expectEqual(core.ok, rig.bind());
    try expect(rig.bd.iface == &abi.ftl_iface);
    try expect(rig.bd.ctx == @as(?*anyopaque, @ptrCast(&rig.ftl)));
}

test "presented caps: the FTL hides erase-before-write" {
    var rig: Rig = .{};
    try expectEqual(core.ok, rig.init());
    try expectEqual(core.ok, rig.bind());
    var caps: core.Caps = .{};
    try expectEqual(core.ok, rig.bd.iface.?.get_caps.?(rig.bd.ctx, &caps));
    try expectEqual(@as(u32, 4), caps.block_count);
    try expect(!caps.must_erase_before_write);
    try expect(!caps.read_only);
    try expectEqual(@as(u8, 0xFF), caps.erase_value);
}

test "vtable: a NULL ctx answers null_ptr on every entry point" {
    var caps: core.Caps = .{};
    var buf: [block]u8 = undefined;
    resetLog();
    try expectEqual(core.err_null_ptr, abi.ftl_iface.read.?(null, 0, 1, &buf));
    try std.testing.expectEqualStrings("ctx must not be nullptr", last_message);
    try expectEqual(core.err_null_ptr, abi.ftl_iface.write.?(null, 0, 1, &buf));
    try expectEqual(core.err_null_ptr, abi.ftl_iface.erase.?(null, 0, 1));
    try expectEqual(core.err_null_ptr, abi.ftl_iface.get_caps.?(null, &caps));
    try expectEqual(core.err_null_ptr, abi.ftl_iface.sync.?(null));
    try expectEqual(@as(usize, 5), log_count);
}

test "read: an unmapped block reads back as the erase value" {
    var rig: Rig = .{};
    try expectEqual(core.ok, rig.init());
    try expectEqual(core.ok, rig.bind());
    var buf: [block]u8 = undefined;
    @memset(&buf, 0x00);
    try expectEqual(core.ok, rig.bd.iface.?.read.?(rig.bd.ctx, 0, 1, &buf));
    for (buf) |byte| try expectEqual(@as(u8, 0xFF), byte);
    try expectEqual(@as(u32, 0), rig.fake.reads);
}

test "read/write: a written block round-trips through the map" {
    var rig: Rig = .{};
    try expectEqual(core.ok, rig.init());
    try expectEqual(core.ok, rig.bind());
    var src: [block]u8 = undefined;
    @memset(&src, 0xA5);
    try expectEqual(core.ok, rig.bd.iface.?.write.?(rig.bd.ctx, 2, 1, &src));

    var phys: u16 = core.unmapped;
    try expectEqual(core.ok, abi.ra8_ftl_phys_of(&rig.ftl, 2, &phys));
    try expect(phys != core.unmapped);
    try expectEqual(core.pstate_live, rig.pblocks[phys].state);

    var dst: [block]u8 = undefined;
    @memset(&dst, 0x00);
    try expectEqual(core.ok, rig.bd.iface.?.read.?(rig.bd.ctx, 2, 1, &dst));
    for (dst) |byte| try expectEqual(@as(u8, 0xA5), byte);
}

test "write: copy-on-write relocates and stales the old block" {
    var rig: Rig = .{};
    try expectEqual(core.ok, rig.init());
    try expectEqual(core.ok, rig.bind());
    var src: [block]u8 = undefined;
    @memset(&src, 0x11);
    try expectEqual(core.ok, rig.bd.iface.?.write.?(rig.bd.ctx, 0, 1, &src));
    var first: u16 = 0;
    try expectEqual(core.ok, abi.ra8_ftl_phys_of(&rig.ftl, 0, &first));

    @memset(&src, 0x22);
    try expectEqual(core.ok, rig.bd.iface.?.write.?(rig.bd.ctx, 0, 1, &src));
    var second: u16 = 0;
    try expectEqual(core.ok, abi.ra8_ftl_phys_of(&rig.ftl, 0, &second));

    try expect(first != second);
    try expectEqual(core.pstate_stale, rig.pblocks[first].state);
    try expectEqual(core.pstate_live, rig.pblocks[second].state);
}

test "write: exhausting the free blocks reclaims the stale ones" {
    var rig: Rig = .{};
    try expectEqual(core.ok, rig.init());
    try expectEqual(core.ok, rig.bind());
    var src: [block]u8 = undefined;
    var round: u8 = 0;
    while (round < 10) : (round += 1) {
        @memset(&src, round);
        try expectEqual(core.ok, rig.bd.iface.?.write.?(rig.bd.ctx, 1, 1, &src));
    }
    var dst: [block]u8 = undefined;
    try expectEqual(core.ok, rig.bd.iface.?.read.?(rig.bd.ctx, 1, 1, &dst));
    for (dst) |byte| try expectEqual(@as(u8, 9), byte);

    var hi: u32 = 0;
    var lo: u32 = 0;
    try expectEqual(core.ok, abi.ra8_ftl_wear_stats(&rig.ftl, &hi, &lo));
    try expect(hi >= 2);
}

test "write: a failed program leaves the prior mapping intact" {
    var rig: Rig = .{};
    try expectEqual(core.ok, rig.init());
    try expectEqual(core.ok, rig.bind());
    var src: [block]u8 = undefined;
    @memset(&src, 0x33);
    try expectEqual(core.ok, rig.bd.iface.?.write.?(rig.bd.ctx, 3, 1, &src));
    var before: u16 = 0;
    try expectEqual(core.ok, abi.ra8_ftl_phys_of(&rig.ftl, 3, &before));

    rig.fake.write_status = 0x302;
    @memset(&src, 0x44);
    try expectEqual(@as(core.Err, 0x302), rig.bd.iface.?.write.?(rig.bd.ctx, 3, 1, &src));
    var after: u16 = 0;
    try expectEqual(core.ok, abi.ra8_ftl_phys_of(&rig.ftl, 3, &after));
    try expectEqual(before, after);
    try expectEqual(core.pstate_live, rig.pblocks[before].state);
}

test "write: a failed erase propagates the device code" {
    var rig: Rig = .{};
    try expectEqual(core.ok, rig.init());
    try expectEqual(core.ok, rig.bind());
    rig.fake.erase_status = 0x303;
    var src: [block]u8 = undefined;
    @memset(&src, 0x55);
    try expectEqual(@as(core.Err, 0x303), rig.bd.iface.?.write.?(rig.bd.ctx, 0, 1, &src));
    try expectEqual(core.unmapped, rig.map[0]);
}

test "bounds: the presented device rejects ranges past the logical size" {
    var rig: Rig = .{};
    try expectEqual(core.ok, rig.init());
    try expectEqual(core.ok, rig.bind());
    var buf: [block * 2]u8 = undefined;
    try expectEqual(core.err_out_of_range, rig.bd.iface.?.read.?(rig.bd.ctx, 3, 2, &buf));
    try expectEqual(core.err_out_of_range, rig.bd.iface.?.write.?(rig.bd.ctx, 0, 5, &buf));
    try expectEqual(core.err_out_of_range, rig.bd.iface.?.erase.?(rig.bd.ctx, 4, 1));
}

test "erase: unmapping makes the block read as the erase value again" {
    var rig: Rig = .{};
    try expectEqual(core.ok, rig.init());
    try expectEqual(core.ok, rig.bind());
    var src: [block]u8 = undefined;
    @memset(&src, 0x66);
    try expectEqual(core.ok, rig.bd.iface.?.write.?(rig.bd.ctx, 1, 1, &src));
    var phys: u16 = 0;
    try expectEqual(core.ok, abi.ra8_ftl_phys_of(&rig.ftl, 1, &phys));

    try expectEqual(core.ok, rig.bd.iface.?.erase.?(rig.bd.ctx, 1, 1));
    try expectEqual(core.pstate_stale, rig.pblocks[phys].state);

    var dst: [block]u8 = undefined;
    @memset(&dst, 0x00);
    try expectEqual(core.ok, rig.bd.iface.?.read.?(rig.bd.ctx, 1, 1, &dst));
    for (dst) |byte| try expectEqual(@as(u8, 0xFF), byte);
}

test "sync: forwarded to the underlying device, faults propagate" {
    var rig: Rig = .{};
    try expectEqual(core.ok, rig.init());
    try expectEqual(core.ok, rig.bind());
    try expectEqual(core.ok, rig.bd.iface.?.sync.?(rig.bd.ctx));
    try expectEqual(@as(u32, 1), rig.fake.syncs);
    rig.fake.sync_status = 0x304;
    try expectEqual(@as(core.Err, 0x304), rig.bd.iface.?.sync.?(rig.bd.ctx));
}

test "read: an underlying read fault propagates" {
    var rig: Rig = .{};
    try expectEqual(core.ok, rig.init());
    try expectEqual(core.ok, rig.bind());
    var src: [block]u8 = undefined;
    @memset(&src, 0x77);
    try expectEqual(core.ok, rig.bd.iface.?.write.?(rig.bd.ctx, 0, 1, &src));
    rig.fake.read_status = 0x305;
    var dst: [block]u8 = undefined;
    try expectEqual(@as(core.Err, 0x305), rig.bd.iface.?.read.?(rig.bd.ctx, 0, 1, &dst));
}

test "wear_stats: NULL guards, then the uninitialised handle" {
    var rig: Rig = .{};
    var hi: u32 = 0;
    var lo: u32 = 0;
    resetLog();
    try expectEqual(core.err_null_ptr, abi.ra8_ftl_wear_stats(null, &hi, &lo));
    try std.testing.expectEqualStrings("ftl must not be nullptr", last_message);
    try expectEqual(core.err_null_ptr, abi.ra8_ftl_wear_stats(&rig.ftl, null, &lo));
    try std.testing.expectEqualStrings("max_out must not be nullptr", last_message);
    try expectEqual(core.err_null_ptr, abi.ra8_ftl_wear_stats(&rig.ftl, &hi, null));
    try std.testing.expectEqualStrings("min_out must not be nullptr", last_message);
    try expectEqual(core.err_not_initialized, abi.ra8_ftl_wear_stats(&rig.ftl, &hi, &lo));
}

test "wear_stats: a cold-start FTL reports zero spread" {
    var rig: Rig = .{};
    try expectEqual(core.ok, rig.init());
    var hi: u32 = 9;
    var lo: u32 = 9;
    try expectEqual(core.ok, abi.ra8_ftl_wear_stats(&rig.ftl, &hi, &lo));
    try expectEqual(@as(u32, 0), hi);
    try expectEqual(@as(u32, 0), lo);
}

test "phys_of: NULL guards, uninitialised handle, then the range check" {
    var rig: Rig = .{};
    var phys: u16 = 0;
    resetLog();
    try expectEqual(core.err_null_ptr, abi.ra8_ftl_phys_of(null, 0, &phys));
    try std.testing.expectEqualStrings("ftl must not be nullptr", last_message);
    try expectEqual(core.err_null_ptr, abi.ra8_ftl_phys_of(&rig.ftl, 0, null));
    try std.testing.expectEqualStrings("phys_out must not be nullptr", last_message);
    try expectEqual(core.err_not_initialized, abi.ra8_ftl_phys_of(&rig.ftl, 0, &phys));

    try expectEqual(core.ok, rig.init());
    try expectEqual(core.err_out_of_range, abi.ra8_ftl_phys_of(&rig.ftl, 4, &phys));
    try expectEqual(core.ok, abi.ra8_ftl_phys_of(&rig.ftl, 3, &phys));
    try expectEqual(core.unmapped, phys);
}

test "multi-block transfers stride 512 bytes per block" {
    var rig: Rig = .{};
    try expectEqual(core.ok, rig.init());
    try expectEqual(core.ok, rig.bind());
    var src: [block * 3]u8 = undefined;
    @memset(src[0..block], 0xA1);
    @memset(src[block .. block * 2], 0xB2);
    @memset(src[block * 2 ..], 0xC3);
    try expectEqual(core.ok, rig.bd.iface.?.write.?(rig.bd.ctx, 0, 3, &src));

    var dst: [block * 3]u8 = undefined;
    @memset(&dst, 0x00);
    try expectEqual(core.ok, rig.bd.iface.?.read.?(rig.bd.ctx, 0, 3, &dst));
    try expectEqual(@as(u8, 0xA1), dst[0]);
    try expectEqual(@as(u8, 0xB2), dst[block]);
    try expectEqual(@as(u8, 0xC3), dst[block * 2]);
}

test "zero-count transfers are accepted and touch nothing" {
    var rig: Rig = .{};
    try expectEqual(core.ok, rig.init());
    try expectEqual(core.ok, rig.bind());
    var buf: [block]u8 = undefined;
    try expectEqual(core.ok, rig.bd.iface.?.read.?(rig.bd.ctx, 0, 0, &buf));
    try expectEqual(core.ok, rig.bd.iface.?.write.?(rig.bd.ctx, 0, 0, &buf));
    try expectEqual(core.ok, rig.bd.iface.?.erase.?(rig.bd.ctx, 0, 0));
    try expectEqual(@as(u32, 0), rig.fake.programs);
    try expectEqual(@as(u32, 0), rig.fake.erases);
}
