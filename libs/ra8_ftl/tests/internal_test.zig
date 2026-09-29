//! Tests for the pure FTL decision core: bounds, sizing, capability
//! validation, wear-levelling selection and the table mutations.

const std = @import("std");
const core = @import("implementation");

const expect = std.testing.expect;
const expectEqual = std.testing.expectEqual;

fn freshPblocks(comptime n: usize) [n]core.Pblock {
    return [_]core.Pblock{.{}} ** n;
}

test "layout: Pblock matches the C ABI" {
    try expectEqual(@as(usize, 8), @sizeOf(core.Pblock));
    try expectEqual(@as(usize, 0), @offsetOf(core.Pblock, "erase_count"));
    try expectEqual(@as(usize, 4), @offsetOf(core.Pblock, "state"));
}

test "layout: Caps matches the C ABI" {
    try expectEqual(@as(usize, 20), @sizeOf(core.Caps));
    try expectEqual(@as(usize, 12), @offsetOf(core.Caps, "logical_block_bytes"));
    try expectEqual(@as(usize, 16), @offsetOf(core.Caps, "read_only"));
}

test "constants keep their C values" {
    try expectEqual(@as(u16, 0xFFFF), core.unmapped);
    try expectEqual(@as(u32, 0xFFFE), core.max_pblocks);
    try expectEqual(@as(u32, 1), core.min_spare);
    try expectEqual(@as(u32, 512), core.block_size_bytes);
    try expectEqual(@as(u8, 0), core.pstate_free);
    try expectEqual(@as(u8, 1), core.pstate_live);
    try expectEqual(@as(u8, 2), core.pstate_stale);
}

test "bounds: a range inside the presented capacity is accepted" {
    try expectEqual(core.ok, core.boundsStatus(16, 0, 16));
    try expectEqual(core.ok, core.boundsStatus(16, 15, 1));
    try expectEqual(core.ok, core.boundsStatus(16, 4, 0));
}

test "bounds: count past the capacity is rejected before the lba check" {
    try expectEqual(core.err_out_of_range, core.boundsStatus(16, 0, 17));
    try expectEqual(core.err_out_of_range, core.boundsStatus(0, 0, 1));
}

test "bounds: a range running off the end is rejected" {
    try expectEqual(core.err_out_of_range, core.boundsStatus(16, 15, 2));
    try expectEqual(core.err_out_of_range, core.boundsStatus(16, 16, 1));
    try expectEqual(core.err_out_of_range, core.boundsStatus(16, 17, 0));
}

test "init sizing: zero logical blocks is invalid_size" {
    try expectEqual(core.err_invalid_size, core.initSizingStatus(0, 8));
}

test "init sizing: physical blocks past the ceiling is invalid_size" {
    try expectEqual(core.err_invalid_size, core.initSizingStatus(1, core.max_pblocks + 1));
    try expectEqual(core.ok, core.initSizingStatus(1, core.max_pblocks));
}

test "check caps: a suitable device yields the erase value" {
    const caps: core.Caps = .{
        .block_count = 24,
        .erase_unit_blocks = 1,
        .program_size_bytes = 512,
        .logical_block_bytes = 512,
        .erase_value = 0xFF,
        .must_erase_before_write = true,
        .read_only = false,
    };
    switch (core.checkCaps(caps, 16, 24)) {
        .value => |v| try expectEqual(@as(u8, 0xFF), v),
        .fault => try expect(false),
    }
}

test "check caps: read-only is judged first" {
    var caps: core.Caps = .{ .block_count = 0, .erase_unit_blocks = 4, .read_only = true };
    switch (core.checkCaps(caps, 16, 24)) {
        .fault => |f| try expectEqual(core.err_invalid_arg, f),
        .value => try expect(false),
    }
    caps.read_only = false;
    switch (core.checkCaps(caps, 16, 24)) {
        .fault => |f| try expectEqual(core.err_invalid_arg, f),
        .value => try expect(false),
    }
}

test "check caps: capacity and the spare-block rule" {
    const small: core.Caps = .{ .block_count = 23, .erase_unit_blocks = 1, .erase_value = 0xFF };
    switch (core.checkCaps(small, 16, 24)) {
        .fault => |f| try expectEqual(core.err_invalid_arg, f),
        .value => try expect(false),
    }
    const no_spare: core.Caps = .{ .block_count = 16, .erase_unit_blocks = 1, .erase_value = 0xFF };
    switch (core.checkCaps(no_spare, 16, 16)) {
        .fault => |f| try expectEqual(core.err_invalid_arg, f),
        .value => try expect(false),
    }
    const one_spare: core.Caps = .{ .block_count = 17, .erase_unit_blocks = 1, .erase_value = 0x00 };
    switch (core.checkCaps(one_spare, 16, 17)) {
        .value => |v| try expectEqual(@as(u8, 0x00), v),
        .fault => try expect(false),
    }
}

test "pick free: no free block reports none" {
    var pblocks = freshPblocks(3);
    for (&pblocks) |*pb| pb.state = core.pstate_live;
    try expect(core.pickFree(&pblocks) == null);
    try expect(core.pickFree(pblocks[0..0]) == null);
}

test "pick free: the least-erased free block wins" {
    var pblocks = freshPblocks(4);
    pblocks[0] = .{ .erase_count = 9, .state = core.pstate_free };
    pblocks[1] = .{ .erase_count = 2, .state = core.pstate_live };
    pblocks[2] = .{ .erase_count = 3, .state = core.pstate_free };
    pblocks[3] = .{ .erase_count = 7, .state = core.pstate_stale };
    try expectEqual(@as(u32, 2), core.pickFree(&pblocks).?);
}

test "pick free: a tie goes to the lowest index" {
    var pblocks = freshPblocks(3);
    pblocks[0] = .{ .erase_count = 5, .state = core.pstate_free };
    pblocks[1] = .{ .erase_count = 5, .state = core.pstate_free };
    pblocks[2] = .{ .erase_count = 5, .state = core.pstate_free };
    try expectEqual(@as(u32, 0), core.pickFree(&pblocks).?);
}

test "presented caps: a free-overwrite device of the logical size" {
    const caps = core.presentedCaps(16, 0xFF);
    try expectEqual(@as(u32, 16), caps.block_count);
    try expectEqual(@as(u32, 1), caps.erase_unit_blocks);
    try expectEqual(@as(u32, 512), caps.program_size_bytes);
    try expectEqual(@as(u16, 512), caps.logical_block_bytes);
    try expectEqual(@as(u8, 0xFF), caps.erase_value);
    try expect(!caps.must_erase_before_write);
    try expect(!caps.read_only);
}

test "reset tables: cold-start state" {
    var map = [_]u16{ 3, 4, 5 };
    var pblocks = freshPblocks(3);
    pblocks[1] = .{ .erase_count = 12, .state = core.pstate_live };
    core.resetTables(&map, &pblocks);
    for (map) |entry| try expectEqual(core.unmapped, entry);
    for (pblocks) |pb| {
        try expectEqual(core.pstate_free, pb.state);
        try expectEqual(@as(u32, 0), pb.erase_count);
    }
}

test "wear stats: max and min across the table" {
    var pblocks = freshPblocks(4);
    pblocks[0].erase_count = 4;
    pblocks[1].erase_count = 9;
    pblocks[2].erase_count = 1;
    pblocks[3].erase_count = 6;
    const wear = core.wearStats(&pblocks);
    try expectEqual(@as(u32, 9), wear.max);
    try expectEqual(@as(u32, 1), wear.min);
}

test "wear stats: an empty table keeps the C's unclamped seed" {
    const pblocks: [0]core.Pblock = .{};
    const wear = core.wearStats(&pblocks);
    try expectEqual(@as(u32, 0), wear.max);
    try expectEqual(core.count_max, wear.min);
}

test "commit write: the superseded block goes stale" {
    var map = [_]u16{ 1, core.unmapped };
    var pblocks = freshPblocks(3);
    pblocks[1].state = core.pstate_live;
    core.commitWrite(&map, &pblocks, 0, 2);
    try expectEqual(@as(u16, 2), map[0]);
    try expectEqual(core.pstate_stale, pblocks[1].state);
    try expectEqual(core.pstate_live, pblocks[2].state);
}

test "commit write: a previously unmapped block stales nothing" {
    var map = [_]u16{core.unmapped};
    var pblocks = freshPblocks(2);
    core.commitWrite(&map, &pblocks, 0, 1);
    try expectEqual(@as(u16, 1), map[0]);
    try expectEqual(core.pstate_free, pblocks[0].state);
    try expectEqual(core.pstate_live, pblocks[1].state);
}

test "unmap one: mapped becomes stale, unmapped is untouched" {
    var map = [_]u16{ 2, core.unmapped };
    var pblocks = freshPblocks(3);
    pblocks[2].state = core.pstate_live;
    core.unmapOne(&map, &pblocks, 0);
    try expectEqual(core.unmapped, map[0]);
    try expectEqual(core.pstate_stale, pblocks[2].state);
    core.unmapOne(&map, &pblocks, 1);
    try expectEqual(core.unmapped, map[1]);
}

test "block offset: 512-byte stride" {
    try expectEqual(@as(usize, 0), core.blockOffset(0));
    try expectEqual(@as(usize, 512), core.blockOffset(1));
    try expectEqual(@as(usize, 512 * 7), core.blockOffset(7));
}
