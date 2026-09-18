//! Pure decision core of the Flash Translation Layer (`ra8_ftl`).
//!
//! Everything here is arithmetic over caller-owned tables: no pointers into
//! the underlying device, no logging, no externs. The ABI membrane in
//! `../ra8_ftl_abi.zig` owns pointer nullability, the presented vtable and
//! the calls down into `ra8_io_blockdev_*`.

const std = @import("std");

/// `ra8_err_t` (see `ra8_err.h`).
pub const Err = u16;

pub const ok: Err = 0;
pub const err_no_mem: Err = 0x102;
pub const err_invalid_arg: Err = 0x103;
pub const err_invalid_size: Err = 0x105;
pub const err_no_data: Err = 0x10A;
pub const err_not_initialized: Err = 0x10F;
pub const err_out_of_range: Err = 0x208;
pub const err_null_ptr: Err = 0x504;

/// `ra8_ftl_const_t` and the implementation-private `ra8_ftl_impl_const_t`.
pub const unmapped: u16 = 0xFFFF;
pub const max_pblocks: u32 = 0xFFFE;
pub const min_spare: u32 = 1;
pub const one_block: u32 = 1;
pub const req_erase_unit: u32 = 1;
pub const count_max: u32 = 0xFFFF_FFFF;

/// `k_ra8_io_block_size_bytes`.
pub const block_size_bytes: u32 = 512;

/// `ra8_ftl_pstate_t`.
pub const pstate_free: u8 = 0;
pub const pstate_live: u8 = 1;
pub const pstate_stale: u8 = 2;

/// `ra8_ftl_pblock_t` -- caller-owned metadata for one physical erase block.
pub const Pblock = extern struct {
    erase_count: u32 = 0,
    state: u8 = 0,
};

/// `ra8_io_blockdev_caps_t`.
pub const Caps = extern struct {
    block_count: u32 = 0,
    erase_unit_blocks: u32 = 0,
    program_size_bytes: u32 = 0,
    logical_block_bytes: u16 = 0,
    erase_value: u8 = 0,
    must_erase_before_write: bool = false,
    read_only: bool = false,
};

comptime {
    // The C ABI shapes, asserted on every target the archive is built for.
    std.debug.assert(@sizeOf(Pblock) == 8);
    std.debug.assert(@alignOf(Pblock) == 4);
    std.debug.assert(@offsetOf(Pblock, "erase_count") == 0);
    std.debug.assert(@offsetOf(Pblock, "state") == 4);

    std.debug.assert(@sizeOf(Caps) == 20);
    std.debug.assert(@offsetOf(Caps, "logical_block_bytes") == 12);
    std.debug.assert(@offsetOf(Caps, "erase_value") == 14);
    std.debug.assert(@offsetOf(Caps, "must_erase_before_write") == 15);
    std.debug.assert(@offsetOf(Caps, "read_only") == 16);
}

/// Result of a check that either yields a value or an `ra8_err_t`.
pub fn Outcome(comptime T: type) type {
    return union(enum) {
        value: T,
        fault: Err,
    };
}

/// `internal_bounds`: two single-condition checks, in the C's order.
///
/// The second comparison is written on the C's `logical_blocks - count`
/// form, which is safe precisely because the first check already rejected
/// `count > logical_blocks`.
pub fn boundsStatus(logical_blocks: u32, lba: u32, count: u32) Err {
    if (count > logical_blocks) {
        return err_out_of_range;
    }
    if (lba > logical_blocks - count) {
        return err_out_of_range;
    }
    return ok;
}

/// Sizing half of `internal_validate_init_args` (the NULL half is pointer
/// nullability and lives in the ABI membrane).
pub fn initSizingStatus(logical_blocks: u32, physical_blocks: u32) Err {
    if (logical_blocks == 0) {
        return err_invalid_size;
    }
    if (physical_blocks > max_pblocks) {
        return err_invalid_size;
    }
    return ok;
}

/// `internal_check_caps`: four single-condition rejections, then the medium
/// erase value is snapshotted. Order matters: read-only is judged before the
/// erase unit, which is judged before capacity, which is judged before spare.
pub fn checkCaps(caps: Caps, logical_blocks: u32, physical_blocks: u32) Outcome(u8) {
    if (caps.read_only) {
        return .{ .fault = err_invalid_arg };
    }
    if (caps.erase_unit_blocks != req_erase_unit) {
        return .{ .fault = err_invalid_arg };
    }
    if (caps.block_count < physical_blocks) {
        return .{ .fault = err_invalid_arg };
    }
    if (physical_blocks < logical_blocks + min_spare) {
        return .{ .fault = err_invalid_arg };
    }
    return .{ .value = caps.erase_value };
}

/// `internal_pick_free`: least-erased FREE block, first index winning a tie.
///
/// The C keeps a `found` flag and skips any candidate whose count is `>=` the
/// incumbent's, so the LOWEST index among equally-worn free blocks is chosen.
pub fn pickFree(pblocks: []const Pblock) ?u32 {
    var found = false;
    var best: u32 = 0;
    var bestc: u32 = 0;
    for (pblocks, 0..) |pb, i| {
        if (pb.state != pstate_free) {
            continue;
        }
        if (found and pb.erase_count >= bestc) {
            continue;
        }
        found = true;
        best = @intCast(i);
        bestc = pb.erase_count;
    }
    return if (found) best else null;
}

/// The FTL's own capabilities: a free-overwrite device of `logical_blocks`
/// blocks that never needs an explicit erase.
pub fn presentedCaps(logical_blocks: u32, erase_value: u8) Caps {
    return .{
        .block_count = logical_blocks,
        .erase_unit_blocks = one_block,
        .program_size_bytes = block_size_bytes,
        .logical_block_bytes = @intCast(block_size_bytes),
        .erase_value = erase_value,
        .must_erase_before_write = false,
        .read_only = false,
    };
}

/// `internal_reset_tables`: cold-start state for both caller tables.
pub fn resetTables(map: []u16, pblocks: []Pblock) void {
    for (map) |*entry| {
        entry.* = unmapped;
    }
    for (pblocks) |*pb| {
        pb.state = pstate_free;
        pb.erase_count = 0;
    }
}

/// Wear-levelling diagnostic. Note the C seeds `lo` with `count_max` and
/// never clamps it, so a zero-length physical table reports
/// `max = 0, min = 0xFFFFFFFF`. Preserved deliberately.
pub const Wear = struct { max: u32, min: u32 };

pub fn wearStats(pblocks: []const Pblock) Wear {
    var hi: u32 = 0;
    var lo: u32 = count_max;
    for (pblocks) |pb| {
        if (pb.erase_count > hi) {
            hi = pb.erase_count;
        }
        if (pb.erase_count < lo) {
            lo = pb.erase_count;
        }
    }
    return .{ .max = hi, .min = lo };
}

/// Commit half of `internal_write_one`, run only after the program succeeded:
/// the superseded physical block goes STALE, the new one LIVE, and the map
/// is re-pointed.
pub fn commitWrite(map: []u16, pblocks: []Pblock, lbn: u32, phys: u32) void {
    const old = map[lbn];
    if (old != unmapped) {
        pblocks[old].state = pstate_stale;
    }
    pblocks[phys].state = pstate_live;
    map[lbn] = @intCast(phys);
}

/// One logical block of `internal_dev_erase`: an unmapped block is left
/// alone, a mapped one is unmapped and its physical block marked STALE.
pub fn unmapOne(map: []u16, pblocks: []Pblock, lbn: u32) void {
    const old = map[lbn];
    if (old != unmapped) {
        pblocks[old].state = pstate_stale;
        map[lbn] = unmapped;
    }
}

/// Byte offset of block `i` inside a `count`-block transfer buffer.
pub fn blockOffset(i: u32) usize {
    return @as(usize, i) * @as(usize, block_size_bytes);
}

// ---------------------------------------------------------------------------
// Checkpoint: the canonical persistent metadata format
// (`src/ra8_ftl_checkpoint.c`). Everything below is pure arithmetic over
// caller-owned storage: the wire codec, CRC-32/ISO-HDLC, the bounded scratch
// window validator, and the encode/commit passes. Pointer nullability, the
// aliasing addresses and the log lines stay in the ABI membrane.
// ---------------------------------------------------------------------------

pub const err_invalid_state: Err = 0x104;
pub const err_not_supported: Err = 0x107;
pub const err_crc_mismatch: Err = 0x405;

/// `ra8_ftl_checkpoint_const_t`.
pub const ck_magic: u32 = 0x4C54_4652;
pub const ck_legacy_magic_le: u32 = 0x4654_4C31;
pub const ck_legacy_magic_swapped: u32 = 0x314C_5446;
pub const ck_version: u16 = 1;
pub const ck_header_bytes: u32 = 20;
pub const ck_crc_bytes: u32 = 4;
pub const ck_fixed_bytes: u32 = 24;
pub const ck_map_entry_bytes: u32 = 2;
pub const ck_pblock_entry_bytes: u32 = 5;
pub const ck_pblock_state_offset: u32 = 4;
pub const ck_off_version: u32 = 4;
pub const ck_off_header_bytes: u32 = 6;
pub const ck_off_total_bytes: u32 = 8;
pub const ck_off_logical_blocks: u32 = 12;
pub const ck_off_physical_blocks: u32 = 16;
pub const ck_scratch_bytes: u32 = 512;
pub const ck_bits_per_byte: u32 = 8;
pub const ck_bitmap_blocks: u32 = 4096;
pub const ck_crc_seed: u32 = 0xFFFF_FFFF;
pub const ck_crc_poly: u32 = 0xEDB8_8320;

/// `internal_get_le16`.
pub fn getLe16(in: []const u8) u16 {
    return @as(u16, in[0]) | (@as(u16, in[1]) << 8);
}

/// `internal_get_le32`.
pub fn getLe32(in: []const u8) u32 {
    return @as(u32, in[0]) | (@as(u32, in[1]) << 8) | (@as(u32, in[2]) << 16) |
        (@as(u32, in[3]) << 24);
}

/// `internal_put_le16`.
pub fn putLe16(out: []u8, value: u16) void {
    out[0] = @truncate(value);
    out[1] = @truncate(value >> 8);
}

/// `internal_put_le32`.
pub fn putLe32(out: []u8, value: u32) void {
    out[0] = @truncate(value);
    out[1] = @truncate(value >> 8);
    out[2] = @truncate(value >> 16);
    out[3] = @truncate(value >> 24);
}

/// `internal_crc32`: CRC-32/ISO-HDLC, bit-by-bit, no lookup table.
///
/// The C builds its mask as `0U - (crc & 1U)`, which relies on unsigned wrap;
/// `-%` keeps that exact shape instead of trapping.
pub fn crc32(data: []const u8) u32 {
    var crc: u32 = ck_crc_seed;
    for (data) |byte| {
        crc ^= byte;
        var bit: u32 = 0;
        while (bit < ck_bits_per_byte) : (bit += 1) {
            const mask: u32 = 0 -% (crc & 1);
            crc = (crc >> 1) ^ (ck_crc_poly & mask);
        }
    }
    return crc ^ ck_crc_seed;
}

/// `internal_ranges_overlap`, taking the two bases as integer addresses.
///
/// Fails closed: an empty span never overlaps, and a non-empty span whose
/// inclusive end would not be representable is reported as overlapping.
pub fn rangesOverlap(first: usize, first_bytes: u32, second: usize, second_bytes: u32) bool {
    if (first_bytes == 0) {
        return false;
    }
    if (second_bytes == 0) {
        return false;
    }
    const top = std.math.maxInt(usize);
    if (first > top - (@as(usize, first_bytes) - 1)) {
        return true;
    }
    if (second > top - (@as(usize, second_bytes) - 1)) {
        return true;
    }
    const first_end = first + @as(usize, first_bytes) - 1;
    const second_end = second + @as(usize, second_bytes) - 1;
    if (first > second_end) {
        return false;
    }
    return second <= first_end;
}

/// `internal_size_values`: the exact wire length, with both multiplications
/// and both sums proven to fit in `uint32_t` first.
pub fn sizeValues(logical_blocks: u32, physical_blocks: u32) Outcome(u32) {
    if (logical_blocks == 0) {
        return .{ .fault = err_invalid_size };
    }
    if (physical_blocks > max_pblocks) {
        return .{ .fault = err_invalid_size };
    }
    if (physical_blocks <= logical_blocks) {
        return .{ .fault = err_invalid_size };
    }
    const top = std.math.maxInt(u32);
    var total: u32 = ck_fixed_bytes;
    if (logical_blocks > (top - total) / ck_map_entry_bytes) {
        return .{ .fault = err_invalid_size };
    }
    total += logical_blocks * ck_map_entry_bytes;
    if (physical_blocks > (top - total) / ck_pblock_entry_bytes) {
        return .{ .fault = err_invalid_size };
    }
    return .{ .value = total + physical_blocks * ck_pblock_entry_bytes };
}

/// `internal_bit_was_set`: sample then set, reporting prior ownership.
pub fn bitWasSet(bitmap: []u8, bit: u32) bool {
    const byte = bit / ck_bits_per_byte;
    const mask = @as(u8, 1) << @intCast(bit % ck_bits_per_byte);
    const was = (bitmap[byte] & mask) != 0;
    bitmap[byte] |= mask;
    return was;
}

/// `internal_bit_is_set`.
pub fn bitIsSet(bitmap: []const u8, bit: u32) bool {
    const byte = bit / ck_bits_per_byte;
    const mask = @as(u8, 1) << @intCast(bit % ck_bits_per_byte);
    return (bitmap[byte] & mask) != 0;
}

/// `internal_window_mark`: an index past the geometry is fatal, an index
/// outside the current window belongs to a later pass, and an in-window index
/// is a duplicate only if some earlier map entry already claimed it.
pub fn windowMark(physical_blocks: u32, scratch: []u8, base: u32, count: u32, phys: u16) Err {
    if (@as(u32, phys) >= physical_blocks) {
        return err_invalid_state;
    }
    if (@as(u32, phys) < base) {
        return ok;
    }
    if (@as(u32, phys) - base >= count) {
        return ok;
    }
    if (bitWasSet(scratch, @as(u32, phys) - base)) {
        return err_invalid_state;
    }
    return ok;
}

/// `internal_native_window`: one bounded window of the LIVE tables.
pub fn nativeWindow(
    map: []const u16,
    pblocks: []const Pblock,
    scratch: []u8,
    base: u32,
    count: u32,
) Err {
    @memset(scratch[0..ck_scratch_bytes], 0);
    const physical: u32 = @intCast(pblocks.len);
    for (map) |phys| {
        if (phys == unmapped) {
            continue;
        }
        const marked = windowMark(physical, scratch, base, count, phys);
        if (marked != ok) {
            return marked;
        }
    }
    var rel: u32 = 0;
    while (rel < count) : (rel += 1) {
        const state = pblocks[base + rel].state;
        if (state > pstate_stale) {
            return err_invalid_state;
        }
        if (bitIsSet(scratch, rel) != (state == pstate_live)) {
            return err_invalid_state;
        }
    }
    return ok;
}

/// `internal_validate_native`: every 4096-block window of the live tables.
pub fn validateNative(map: []const u16, pblocks: []const Pblock, scratch: []u8) Err {
    const physical: u32 = @intCast(pblocks.len);
    var base: u32 = 0;
    while (base < physical) : (base += ck_bitmap_blocks) {
        const left = physical - base;
        const count = if (left < ck_bitmap_blocks) left else ck_bitmap_blocks;
        const valid = nativeWindow(map, pblocks, scratch, base, count);
        if (valid != ok) {
            return valid;
        }
    }
    return ok;
}

/// `internal_wire_window`: the same window pass, read straight off the wire.
pub fn wireWindow(
    buf: []const u8,
    logical_blocks: u32,
    physical_blocks: u32,
    map_offset: u32,
    pb_offset: u32,
    scratch: []u8,
    base: u32,
    count: u32,
) Err {
    @memset(scratch[0..ck_scratch_bytes], 0);
    var lbn: u32 = 0;
    while (lbn < logical_blocks) : (lbn += 1) {
        const entry = map_offset + (lbn * ck_map_entry_bytes);
        const phys = getLe16(buf[entry..]);
        if (phys == unmapped) {
            continue;
        }
        const marked = windowMark(physical_blocks, scratch, base, count, phys);
        if (marked != ok) {
            return marked;
        }
    }
    var rel: u32 = 0;
    while (rel < count) : (rel += 1) {
        const entry = pb_offset + ((base + rel) * ck_pblock_entry_bytes);
        const state = buf[entry + ck_pblock_state_offset];
        if (state > pstate_stale) {
            return err_invalid_state;
        }
        if (bitIsSet(scratch, rel) != (state == pstate_live)) {
            return err_invalid_state;
        }
    }
    return ok;
}

/// `internal_validate_wire`: the whole payload, without touching live state.
pub fn validateWire(
    buf: []const u8,
    logical_blocks: u32,
    physical_blocks: u32,
    scratch: []u8,
) Err {
    const map_offset = ck_header_bytes;
    const pb_offset = map_offset + (logical_blocks * ck_map_entry_bytes);
    var base: u32 = 0;
    while (base < physical_blocks) : (base += ck_bitmap_blocks) {
        const left = physical_blocks - base;
        const count = if (left < ck_bitmap_blocks) left else ck_bitmap_blocks;
        const valid = wireWindow(
            buf,
            logical_blocks,
            physical_blocks,
            map_offset,
            pb_offset,
            scratch,
            base,
            count,
        );
        if (valid != ok) {
            return valid;
        }
    }
    return ok;
}

/// `internal_validate_header`: magic (both legacy byte orders refused as
/// not_supported), version, header size, exact length, geometry, then CRC.
pub fn validateHeader(
    buf: []const u8,
    buf_len: u32,
    need: u32,
    logical_blocks: u32,
    physical_blocks: u32,
) Err {
    if (buf_len < ck_crc_bytes) {
        return err_invalid_size;
    }
    const magic = getLe32(buf);
    if (magic == ck_legacy_magic_le) {
        return err_not_supported;
    }
    if (magic == ck_legacy_magic_swapped) {
        return err_not_supported;
    }
    if (magic != ck_magic) {
        return err_invalid_state;
    }
    if (buf_len < ck_fixed_bytes) {
        return err_invalid_size;
    }
    if (getLe16(buf[ck_off_version..]) != ck_version) {
        return err_not_supported;
    }
    if (getLe16(buf[ck_off_header_bytes..]) != @as(u16, @intCast(ck_header_bytes))) {
        return err_invalid_size;
    }
    if (getLe32(buf[ck_off_total_bytes..]) != buf_len) {
        return err_invalid_size;
    }
    if (buf_len != need) {
        return err_invalid_size;
    }
    if (getLe32(buf[ck_off_logical_blocks..]) != logical_blocks) {
        return err_invalid_arg;
    }
    if (getLe32(buf[ck_off_physical_blocks..]) != physical_blocks) {
        return err_invalid_arg;
    }
    const crc_offset = buf_len - ck_crc_bytes;
    return if (crc32(buf[0..crc_offset]) == getLe32(buf[crc_offset..]))
        ok
    else
        err_crc_mismatch;
}

/// `internal_encode`: the validated live state, field by field, little-endian.
pub fn encode(map: []const u16, pblocks: []const Pblock, buf: []u8, need: u32) void {
    putLe32(buf, ck_magic);
    putLe16(buf[ck_off_version..], ck_version);
    putLe16(buf[ck_off_header_bytes..], @intCast(ck_header_bytes));
    putLe32(buf[ck_off_total_bytes..], need);
    putLe32(buf[ck_off_logical_blocks..], @intCast(map.len));
    putLe32(buf[ck_off_physical_blocks..], @intCast(pblocks.len));
    var offset: u32 = ck_header_bytes;
    for (map) |entry| {
        putLe16(buf[offset..], entry);
        offset += ck_map_entry_bytes;
    }
    for (pblocks) |pb| {
        putLe32(buf[offset..], pb.erase_count);
        buf[offset + ck_pblock_state_offset] = pb.state;
        offset += ck_pblock_entry_bytes;
    }
    const sum = crc32(buf[0..offset]);
    putLe32(buf[offset..], sum);
}

/// `internal_decode_commit`: run only once every validation pass has passed.
pub fn decodeCommit(map: []u16, pblocks: []Pblock, buf: []const u8) void {
    var offset: u32 = ck_header_bytes;
    for (map) |*entry| {
        entry.* = getLe16(buf[offset..]);
        offset += ck_map_entry_bytes;
    }
    for (pblocks) |*pb| {
        pb.erase_count = getLe32(buf[offset..]);
        pb.state = buf[offset + ck_pblock_state_offset];
        offset += ck_pblock_entry_bytes;
    }
}
