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

// ---------------------------------------------------------------------------
// Checkpoint core.
// ---------------------------------------------------------------------------

fn sized(logical: u32, physical: u32) !u32 {
    return switch (core.sizeValues(logical, physical)) {
        .value => |v| v,
        .fault => error.Unexpected,
    };
}

test "checkpoint constants keep their C values" {
    try expectEqual(@as(u32, 0x4C544652), core.ck_magic);
    try expectEqual(@as(u32, 0x46544C31), core.ck_legacy_magic_le);
    try expectEqual(@as(u32, 0x314C5446), core.ck_legacy_magic_swapped);
    try expectEqual(@as(u16, 1), core.ck_version);
    try expectEqual(@as(u32, 20), core.ck_header_bytes);
    try expectEqual(@as(u32, 24), core.ck_fixed_bytes);
    try expectEqual(@as(u32, 5), core.ck_pblock_entry_bytes);
    try expectEqual(@as(u32, 512), core.ck_scratch_bytes);
    try expectEqual(@as(u32, 4096), core.ck_bitmap_blocks);
    try expectEqual(@as(u16, 0x104), core.err_invalid_state);
    try expectEqual(@as(u16, 0x107), core.err_not_supported);
    try expectEqual(@as(u16, 0x405), core.err_crc_mismatch);
}

test "wire codec: little-endian round trips independent of host order" {
    var buf = [_]u8{0} ** 8;
    core.putLe16(buf[0..], 0xBEEF);
    core.putLe32(buf[2..], 0xDEADC0DE);
    try expectEqual(@as(u8, 0xEF), buf[0]);
    try expectEqual(@as(u8, 0xBE), buf[1]);
    try expectEqual(@as(u8, 0xDE), buf[2]);
    try expectEqual(@as(u8, 0xC0), buf[3]);
    try expectEqual(@as(u8, 0xAD), buf[4]);
    try expectEqual(@as(u8, 0xDE), buf[5]);
    try expectEqual(@as(u16, 0xBEEF), core.getLe16(buf[0..]));
    try expectEqual(@as(u32, 0xDEADC0DE), core.getLe32(buf[2..]));
}

test "wire codec: the saturated ends decode as the C documents" {
    const zeros = [_]u8{0} ** 4;
    const ones = [_]u8{0xFF} ** 4;
    try expectEqual(@as(u16, 0), core.getLe16(zeros[0..]));
    try expectEqual(@as(u32, 0), core.getLe32(zeros[0..]));
    try expectEqual(@as(u16, 0xFFFF), core.getLe16(ones[0..]));
    try expectEqual(@as(u32, 0xFFFFFFFF), core.getLe32(ones[0..]));
}

test "crc32: the ISO-HDLC check vector and the empty span" {
    try expectEqual(@as(u32, 0xCBF43926), core.crc32("123456789"));
    try expectEqual(@as(u32, 0), core.crc32(""));
}

test "crc32: one flipped bit changes the checksum" {
    var payload = [_]u8{ 1, 2, 3, 4, 5, 6, 7, 8 };
    const before = core.crc32(payload[0..]);
    payload[3] ^= 0x01;
    try expect(before != core.crc32(payload[0..]));
}

test "ranges overlap: an empty span never overlaps" {
    try expect(!core.rangesOverlap(0x1000, 0, 0x1000, 16));
    try expect(!core.rangesOverlap(0x1000, 16, 0x1000, 0));
}

test "ranges overlap: touching, nested and disjoint spans" {
    try expect(core.rangesOverlap(0x1000, 16, 0x100F, 1));
    try expect(!core.rangesOverlap(0x1000, 16, 0x1010, 1));
    try expect(core.rangesOverlap(0x1000, 16, 0x1004, 4));
    try expect(!core.rangesOverlap(0x1010, 16, 0x1000, 16));
}

test "ranges overlap: an unrepresentable end fails closed" {
    const top = std.math.maxInt(usize);
    try expect(core.rangesOverlap(top, 2, 0x10, 1));
    try expect(core.rangesOverlap(0x10, 1, top, 2));
}

test "size values: the geometry rejections, in the C's order" {
    try expectEqual(core.err_invalid_size, core.sizeValues(0, 8).fault);
    try expectEqual(core.err_invalid_size, core.sizeValues(4, core.max_pblocks + 1).fault);
    try expectEqual(core.err_invalid_size, core.sizeValues(8, 8).fault);
    try expectEqual(core.err_invalid_size, core.sizeValues(9, 8).fault);
}

test "size values: header, map and physical records add up exactly" {
    try expectEqual(@as(u32, 24 + 4 * 2 + 6 * 5), try sized(4, 6));
    try expectEqual(@as(u32, 24 + 1 * 2 + 2 * 5), try sized(1, 2));
}

test "bitmap: a bit reports its prior state and then stays set" {
    var bitmap = [_]u8{0} ** 8;
    try expect(!core.bitIsSet(bitmap[0..], 9));
    try expect(!core.bitWasSet(bitmap[0..], 9));
    try expect(core.bitIsSet(bitmap[0..], 9));
    try expect(core.bitWasSet(bitmap[0..], 9));
    try expectEqual(@as(u8, 0x02), bitmap[1]);
    try expectEqual(@as(u8, 0), bitmap[0]);
}

test "window mark: out of range is fatal, out of window is a later pass" {
    var scratch = [_]u8{0} ** 512;
    try expectEqual(core.err_invalid_state, core.windowMark(6, scratch[0..], 0, 6, 6));
    try expectEqual(core.ok, core.windowMark(8192, scratch[0..], 4096, 4096, 3));
    try expectEqual(core.ok, core.windowMark(8192, scratch[0..], 0, 4096, 5000));
    try expectEqual(@as(u8, 0), scratch[0]);
}

test "window mark: a second claim on one physical block is a duplicate" {
    var scratch = [_]u8{0} ** 512;
    try expectEqual(core.ok, core.windowMark(6, scratch[0..], 0, 6, 2));
    try expectEqual(core.err_invalid_state, core.windowMark(6, scratch[0..], 0, 6, 2));
}

test "validate native: a consistent cold-start table passes" {
    var map = [_]u16{core.unmapped} ** 4;
    var pblocks = freshPblocks(6);
    var scratch = [_]u8{0} ** 512;
    try expectEqual(core.ok, core.validateNative(map[0..], pblocks[0..], scratch[0..]));
}

test "validate native: a mapped block must be LIVE and a LIVE block mapped" {
    var map = [_]u16{core.unmapped} ** 4;
    var pblocks = freshPblocks(6);
    var scratch = [_]u8{0} ** 512;

    map[1] = 3;
    try expectEqual(core.err_invalid_state, core.validateNative(map[0..], pblocks[0..], scratch[0..]));
    pblocks[3].state = core.pstate_live;
    try expectEqual(core.ok, core.validateNative(map[0..], pblocks[0..], scratch[0..]));

    pblocks[4].state = core.pstate_live;
    try expectEqual(core.err_invalid_state, core.validateNative(map[0..], pblocks[0..], scratch[0..]));
    pblocks[4].state = core.pstate_stale;
    try expectEqual(core.ok, core.validateNative(map[0..], pblocks[0..], scratch[0..]));
}

test "validate native: an unknown physical state is rejected" {
    var map = [_]u16{core.unmapped} ** 4;
    var pblocks = freshPblocks(6);
    var scratch = [_]u8{0} ** 512;
    pblocks[2].state = core.pstate_stale + 1;
    try expectEqual(core.err_invalid_state, core.validateNative(map[0..], pblocks[0..], scratch[0..]));
}

test "validate native: two logical blocks may not share a physical block" {
    var map = [_]u16{core.unmapped} ** 4;
    var pblocks = freshPblocks(6);
    var scratch = [_]u8{0} ** 512;
    map[0] = 1;
    map[2] = 1;
    pblocks[1].state = core.pstate_live;
    try expectEqual(core.err_invalid_state, core.validateNative(map[0..], pblocks[0..], scratch[0..]));
}

test "encode/decode: a checkpoint round trips through the wire format" {
    var map = [_]u16{ 2, core.unmapped, 5, core.unmapped };
    var pblocks = freshPblocks(6);
    pblocks[2] = .{ .erase_count = 7, .state = core.pstate_live };
    pblocks[5] = .{ .erase_count = 0x01020304, .state = core.pstate_live };
    pblocks[4] = .{ .erase_count = 1, .state = core.pstate_stale };

    const need = try sized(4, 6);
    var wire = [_]u8{0} ** 64;
    core.encode(map[0..], pblocks[0..], wire[0..need], need);

    try expectEqual(core.ck_magic, core.getLe32(wire[0..]));
    try expectEqual(core.ck_version, core.getLe16(wire[core.ck_off_version..]));
    try expectEqual(@as(u16, 20), core.getLe16(wire[core.ck_off_header_bytes..]));
    try expectEqual(need, core.getLe32(wire[core.ck_off_total_bytes..]));
    try expectEqual(@as(u32, 4), core.getLe32(wire[core.ck_off_logical_blocks..]));
    try expectEqual(@as(u32, 6), core.getLe32(wire[core.ck_off_physical_blocks..]));

    var back_map = [_]u16{0} ** 4;
    var back_pblocks = freshPblocks(6);
    core.decodeCommit(back_map[0..], back_pblocks[0..], wire[0..need]);
    try expectEqual(map, back_map);
    for (pblocks, back_pblocks) |want, got| {
        try expectEqual(want.erase_count, got.erase_count);
        try expectEqual(want.state, got.state);
    }
}

test "validate header: the happy path accepts an encoded checkpoint" {
    var map = [_]u16{core.unmapped} ** 4;
    var pblocks = freshPblocks(6);
    const need = try sized(4, 6);
    var wire = [_]u8{0} ** 64;
    core.encode(map[0..], pblocks[0..], wire[0..need], need);
    try expectEqual(core.ok, core.validateHeader(wire[0..need], need, need, 4, 6));
}

test "validate header: both legacy byte orders are not_supported" {
    var wire = [_]u8{0} ** 64;
    core.putLe32(wire[0..], core.ck_legacy_magic_le);
    try expectEqual(core.err_not_supported, core.validateHeader(wire[0..24], 24, 24, 4, 6));
    core.putLe32(wire[0..], core.ck_legacy_magic_swapped);
    try expectEqual(core.err_not_supported, core.validateHeader(wire[0..24], 24, 24, 4, 6));
}

test "validate header: the rejection order from magic to CRC" {
    var map = [_]u16{core.unmapped} ** 4;
    var pblocks = freshPblocks(6);
    const need = try sized(4, 6);
    var wire = [_]u8{0} ** 64;
    core.encode(map[0..], pblocks[0..], wire[0..need], need);

    // Too short to even hold a trailer.
    try expectEqual(core.err_invalid_size, core.validateHeader(wire[0..2], 2, need, 4, 6));
    // Foreign magic.
    var alien = wire;
    core.putLe32(alien[0..], 0x11223344);
    try expectEqual(core.err_invalid_state, core.validateHeader(alien[0..need], need, need, 4, 6));
    // Right magic, but shorter than the fixed header plus trailer.
    var stub = [_]u8{0} ** 8;
    core.putLe32(stub[0..], core.ck_magic);
    try expectEqual(core.err_invalid_size, core.validateHeader(stub[0..8], 8, need, 4, 6));
    // Unknown version.
    var versioned = wire;
    core.putLe16(versioned[core.ck_off_version..], 2);
    try expectEqual(core.err_not_supported, core.validateHeader(versioned[0..need], need, need, 4, 6));
    // Header size that is not the version-1 header.
    var header = wire;
    core.putLe16(header[core.ck_off_header_bytes..], 21);
    try expectEqual(core.err_invalid_size, core.validateHeader(header[0..need], need, need, 4, 6));
    // Stored length disagreeing with the accessible length.
    var total = wire;
    core.putLe32(total[core.ck_off_total_bytes..], need + 1);
    try expectEqual(core.err_invalid_size, core.validateHeader(total[0..need], need, need, 4, 6));
    // Geometry that is not this FTL's.
    var geometry = wire;
    core.putLe32(geometry[core.ck_off_logical_blocks..], 3);
    try expectEqual(core.err_invalid_arg, core.validateHeader(geometry[0..need], need, need, 4, 6));
    geometry = wire;
    core.putLe32(geometry[core.ck_off_physical_blocks..], 7);
    try expectEqual(core.err_invalid_arg, core.validateHeader(geometry[0..need], need, need, 4, 6));
    // A payload byte edited without refreshing the trailer.
    var corrupt = wire;
    corrupt[core.ck_header_bytes] ^= 0xFF;
    try expectEqual(core.err_crc_mismatch, core.validateHeader(corrupt[0..need], need, need, 4, 6));
}

test "validate header: a length that is not this geometry's is invalid_size" {
    var map = [_]u16{core.unmapped} ** 4;
    var pblocks = freshPblocks(6);
    const need = try sized(4, 6);
    var wire = [_]u8{0} ** 64;
    core.encode(map[0..], pblocks[0..], wire[0..need], need);
    try expectEqual(core.err_invalid_size, core.validateHeader(wire[0..need], need, need + 5, 4, 6));
}

test "validate wire: the payload invariants are checked without live state" {
    var map = [_]u16{ 2, core.unmapped, core.unmapped, core.unmapped };
    var pblocks = freshPblocks(6);
    pblocks[2].state = core.pstate_live;
    const need = try sized(4, 6);
    var wire = [_]u8{0} ** 64;
    var scratch = [_]u8{0} ** 512;
    core.encode(map[0..], pblocks[0..], wire[0..need], need);
    try expectEqual(core.ok, core.validateWire(wire[0..need], 4, 6, scratch[0..]));

    // A wire map entry pointing outside the geometry.
    var out_of_range = wire;
    core.putLe16(out_of_range[core.ck_header_bytes..], 6);
    try expectEqual(
        core.err_invalid_state,
        core.validateWire(out_of_range[0..need], 4, 6, scratch[0..]),
    );

    // A wire physical state byte that names no lifecycle state.
    var bad_state = wire;
    const pb_offset = core.ck_header_bytes + 4 * core.ck_map_entry_bytes;
    bad_state[pb_offset + core.ck_pblock_state_offset] = 9;
    try expectEqual(
        core.err_invalid_state,
        core.validateWire(bad_state[0..need], 4, 6, scratch[0..]),
    );
}
