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
