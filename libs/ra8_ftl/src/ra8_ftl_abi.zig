//! C ABI surface of the Flash Translation Layer (`libs/ra8_ftl`).
//!
//! `inc/ra8_ftl.h` is unchanged: this file exports the same four public
//! `ra8_ftl_*` symbols `ra8_ftl.c` did, with the same guard order, the same
//! codes, the same log lines and the same presented `ra8_io_blockdev_iface`
//! vtable. Every decision lives in `internal/root.zig`; what stays here is
//! pointer nullability, the calls down into the underlying device, and the
//! writes into caller-owned storage.
//!
//! External symbols: `ra8_log_emit_error` plus the five `ra8_io_blockdev_*`
//! front-door helpers, all of which live in `ra8_io` (still C).

const std = @import("std");
pub const core = @import("internal/root.zig");

const Err = core.Err;

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;

const tag: [*:0]const u8 = "ra8_ftl";

// ---------------------------------------------------------------------------
// The underlying device (ra8_io_blockdev.h / ra8_io_blockdev_backend.h).
// ---------------------------------------------------------------------------

/// struct ra8_io_blockdev_iface.
pub const BlockdevIface = extern struct {
    read: ?*const fn (?*anyopaque, u32, u32, [*]u8) callconv(.c) Err = null,
    write: ?*const fn (?*anyopaque, u32, u32, [*]const u8) callconv(.c) Err = null,
    erase: ?*const fn (?*anyopaque, u32, u32) callconv(.c) Err = null,
    get_caps: ?*const fn (?*const anyopaque, *core.Caps) callconv(.c) Err = null,
    sync: ?*const fn (?*anyopaque) callconv(.c) Err = null,
};

/// ra8_io_blockdev_t.
pub const Blockdev = extern struct {
    iface: ?*const BlockdevIface = null,
    ctx: ?*anyopaque = null,
};

/// ra8_ftl_t -- the caller-allocated handle.
pub const Ftl = extern struct {
    raw: ?*const Blockdev = null,
    map: ?[*]u16 = null,
    pblocks: ?[*]core.Pblock = null,
    scratch: ?[*]u8 = null,
    logical_blocks: u32 = 0,
    physical_blocks: u32 = 0,
    erase_value: u8 = 0,
};

comptime {
    const ptr = @sizeOf(usize);
    std.debug.assert(@offsetOf(Blockdev, "ctx") == ptr);
    std.debug.assert(@sizeOf(Blockdev) == ptr * 2);
    std.debug.assert(@offsetOf(Ftl, "logical_blocks") == ptr * 4);
    std.debug.assert(@offsetOf(Ftl, "physical_blocks") == ptr * 4 + 4);
    std.debug.assert(@offsetOf(Ftl, "erase_value") == ptr * 4 + 8);
    std.debug.assert(@sizeOf(Ftl) == std.mem.alignForward(usize, ptr * 4 + 9, @alignOf(Ftl)));
}

extern fn ra8_io_blockdev_read(bd: *const Blockdev, lba: u32, count: u32, buf: [*]u8) Err;
extern fn ra8_io_blockdev_write(bd: *const Blockdev, lba: u32, count: u32, buf: [*]const u8) Err;
extern fn ra8_io_blockdev_erase(bd: *const Blockdev, lba: u32, count: u32) Err;
extern fn ra8_io_blockdev_sync(bd: *const Blockdev) Err;
extern fn ra8_io_blockdev_get_caps(bd: *const Blockdev, out: *core.Caps) Err;

// ---------------------------------------------------------------------------
// Physical-block allocation (wear-levelling + reclamation).
// ---------------------------------------------------------------------------

fn mapSlice(ftl: *const Ftl) []u16 {
    return ftl.map.?[0..ftl.logical_blocks];
}

fn pblockSlice(ftl: *const Ftl) []core.Pblock {
    return ftl.pblocks.?[0..ftl.physical_blocks];
}

/// internal_reclaim_stale: erase every STALE block back to FREE.
fn reclaimStale(ftl: *Ftl) Err {
    const raw = ftl.raw orelse {
        ra8_log_emit_error(tag, "raw must not be nullptr");
        return core.err_null_ptr;
    };
    const pblocks = pblockSlice(ftl);
    for (pblocks, 0..) |*pb, i| {
        if (pb.state != core.pstate_stale) {
            continue;
        }
        const e = ra8_io_blockdev_erase(raw, @intCast(i), core.one_block);
        if (e != core.ok) {
            return e;
        }
        pb.erase_count += 1;
        pb.state = core.pstate_free;
    }
    return core.ok;
}

/// internal_alloc_blank: pick the least-erased FREE block, reclaiming once if
/// none is free, then erase it so the caller programs a blank block.
fn allocBlank(ftl: *Ftl, out: *u32) Err {
    var phys = core.pickFree(pblockSlice(ftl));
    if (phys == null) {
        const rec = reclaimStale(ftl);
        if (rec != core.ok) {
            return rec;
        }
        phys = core.pickFree(pblockSlice(ftl));
    }
    const chosen = phys orelse return core.err_no_mem;

    const raw = ftl.raw orelse {
        ra8_log_emit_error(tag, "raw must not be nullptr");
        return core.err_null_ptr;
    };
    const e = ra8_io_blockdev_erase(raw, chosen, core.one_block);
    if (e != core.ok) {
        return e;
    }
    pblockSlice(ftl)[chosen].erase_count += 1;
    out.* = chosen;
    return core.ok;
}

/// internal_read_one: unmapped blocks synthesise the erase value.
fn readOne(ftl: *const Ftl, lbn: u32, dst: [*]u8) Err {
    const phys = mapSlice(ftl)[lbn];
    if (phys == core.unmapped) {
        @memset(dst[0..core.block_size_bytes], ftl.erase_value);
        return core.ok;
    }
    const raw = ftl.raw orelse {
        ra8_log_emit_error(tag, "raw must not be nullptr");
        return core.err_null_ptr;
    };
    return ra8_io_blockdev_read(raw, phys, core.one_block, dst);
}

/// internal_write_one: copy-on-write relocation.
fn writeOne(ftl: *Ftl, lbn: u32, src: [*]const u8) Err {
    var phys: u32 = 0;
    const alloc = allocBlank(ftl, &phys);
    if (alloc != core.ok) {
        return alloc;
    }
    const raw = ftl.raw orelse {
        ra8_log_emit_error(tag, "raw must not be nullptr");
        return core.err_null_ptr;
    };
    const prog = ra8_io_blockdev_write(raw, phys, core.one_block, src);
    if (prog != core.ok) {
        return prog;
    }
    core.commitWrite(mapSlice(ftl), pblockSlice(ftl), lbn, phys);
    return core.ok;
}

// ---------------------------------------------------------------------------
// The presented vtable.
// ---------------------------------------------------------------------------

fn devRead(ctx: ?*anyopaque, lba: u32, count: u32, buf: [*]u8) callconv(.c) Err {
    const raw_ctx = ctx orelse {
        ra8_log_emit_error(tag, "ctx must not be nullptr");
        return core.err_null_ptr;
    };
    const ftl: *const Ftl = @ptrCast(@alignCast(raw_ctx));
    const b = core.boundsStatus(ftl.logical_blocks, lba, count);
    if (b != core.ok) {
        return b;
    }
    var i: u32 = 0;
    while (i < count) : (i += 1) {
        const e = readOne(ftl, lba + i, buf + core.blockOffset(i));
        if (e != core.ok) {
            return e;
        }
    }
    return core.ok;
}

fn devWrite(ctx: ?*anyopaque, lba: u32, count: u32, buf: [*]const u8) callconv(.c) Err {
    const raw_ctx = ctx orelse {
        ra8_log_emit_error(tag, "ctx must not be nullptr");
        return core.err_null_ptr;
    };
    const ftl: *Ftl = @ptrCast(@alignCast(raw_ctx));
    const b = core.boundsStatus(ftl.logical_blocks, lba, count);
    if (b != core.ok) {
        return b;
    }
    var i: u32 = 0;
    while (i < count) : (i += 1) {
        const e = writeOne(ftl, lba + i, buf + core.blockOffset(i));
        if (e != core.ok) {
            return e;
        }
    }
    return core.ok;
}

fn devErase(ctx: ?*anyopaque, lba: u32, count: u32) callconv(.c) Err {
    const raw_ctx = ctx orelse {
        ra8_log_emit_error(tag, "ctx must not be nullptr");
        return core.err_null_ptr;
    };
    const ftl: *Ftl = @ptrCast(@alignCast(raw_ctx));
    const b = core.boundsStatus(ftl.logical_blocks, lba, count);
    if (b != core.ok) {
        return b;
    }
    var i: u32 = 0;
    while (i < count) : (i += 1) {
        core.unmapOne(mapSlice(ftl), pblockSlice(ftl), lba + i);
    }
    return core.ok;
}

fn devGetCaps(ctx: ?*const anyopaque, out: *core.Caps) callconv(.c) Err {
    const raw_ctx = ctx orelse {
        ra8_log_emit_error(tag, "ctx must not be nullptr");
        return core.err_null_ptr;
    };
    const ftl: *const Ftl = @ptrCast(@alignCast(raw_ctx));
    out.* = core.presentedCaps(ftl.logical_blocks, ftl.erase_value);
    return core.ok;
}

fn devSync(ctx: ?*anyopaque) callconv(.c) Err {
    const raw_ctx = ctx orelse {
        ra8_log_emit_error(tag, "ctx must not be nullptr");
        return core.err_null_ptr;
    };
    const ftl: *const Ftl = @ptrCast(@alignCast(raw_ctx));
    const raw = ftl.raw orelse {
        ra8_log_emit_error(tag, "raw must not be nullptr");
        return core.err_null_ptr;
    };
    return ra8_io_blockdev_sync(raw);
}

/// The presented FTL vtable -- a clean free-overwrite block device.
pub const ftl_iface: BlockdevIface = .{
    .read = devRead,
    .write = devWrite,
    .erase = devErase,
    .get_caps = devGetCaps,
    .sync = devSync,
};

// ---------------------------------------------------------------------------
// Public API.
// ---------------------------------------------------------------------------

pub export fn ra8_ftl_init(
    bd: ?*Ftl,
    raw: ?*const Blockdev,
    map: ?[*]u16,
    logical_blocks: u32,
    pblocks: ?[*]core.Pblock,
    physical_blocks: u32,
    scratch: ?[*]u8,
) callconv(.c) Err {
    // internal_validate_init_args: five NULL guards in declaration order,
    // then the two sizing rejections.
    const handle = bd orelse {
        ra8_log_emit_error(tag, "bd must not be nullptr");
        return core.err_null_ptr;
    };
    const device = raw orelse {
        ra8_log_emit_error(tag, "raw must not be nullptr");
        return core.err_null_ptr;
    };
    const map_ptr = map orelse {
        ra8_log_emit_error(tag, "map must not be nullptr");
        return core.err_null_ptr;
    };
    const pblock_ptr = pblocks orelse {
        ra8_log_emit_error(tag, "pblocks must not be nullptr");
        return core.err_null_ptr;
    };
    const scratch_ptr = scratch orelse {
        ra8_log_emit_error(tag, "scratch must not be nullptr");
        return core.err_null_ptr;
    };
    const sizing = core.initSizingStatus(logical_blocks, physical_blocks);
    if (sizing != core.ok) {
        return sizing;
    }

    var caps: core.Caps = .{};
    const cq = ra8_io_blockdev_get_caps(device, &caps);
    if (cq != core.ok) {
        return cq;
    }
    const checked = core.checkCaps(caps, logical_blocks, physical_blocks);
    const erase_value = switch (checked) {
        .fault => |f| return f,
        .value => |v| v,
    };

    handle.raw = device;
    handle.map = map_ptr;
    handle.pblocks = pblock_ptr;
    handle.scratch = scratch_ptr;
    handle.logical_blocks = logical_blocks;
    handle.physical_blocks = physical_blocks;
    handle.erase_value = erase_value;

    // internal_reset_tables re-checks its two pointers; both were just
    // proven non-NULL above, so only the table writes are observable.
    core.resetTables(mapSlice(handle), pblockSlice(handle));
    return core.ok;
}

pub export fn ra8_ftl_as_blockdev(ftl: ?*Ftl, out: ?*Blockdev) callconv(.c) Err {
    const handle = ftl orelse {
        ra8_log_emit_error(tag, "ftl must not be nullptr");
        return core.err_null_ptr;
    };
    const target = out orelse {
        ra8_log_emit_error(tag, "out must not be nullptr");
        return core.err_null_ptr;
    };
    if (handle.raw == null) {
        return core.err_not_initialized;
    }
    target.iface = &ftl_iface;
    target.ctx = handle;
    return core.ok;
}

pub export fn ra8_ftl_wear_stats(ftl: ?*const Ftl, max_out: ?*u32, min_out: ?*u32) callconv(.c) Err {
    const handle = ftl orelse {
        ra8_log_emit_error(tag, "ftl must not be nullptr");
        return core.err_null_ptr;
    };
    const hi_out = max_out orelse {
        ra8_log_emit_error(tag, "max_out must not be nullptr");
        return core.err_null_ptr;
    };
    const lo_out = min_out orelse {
        ra8_log_emit_error(tag, "min_out must not be nullptr");
        return core.err_null_ptr;
    };
    if (handle.pblocks == null) {
        return core.err_not_initialized;
    }
    const wear = core.wearStats(pblockSlice(handle));
    hi_out.* = wear.max;
    lo_out.* = wear.min;
    return core.ok;
}

pub export fn ra8_ftl_phys_of(ftl: ?*const Ftl, lbn: u32, phys_out: ?*u16) callconv(.c) Err {
    const handle = ftl orelse {
        ra8_log_emit_error(tag, "ftl must not be nullptr");
        return core.err_null_ptr;
    };
    const out = phys_out orelse {
        ra8_log_emit_error(tag, "phys_out must not be nullptr");
        return core.err_null_ptr;
    };
    if (handle.map == null) {
        return core.err_not_initialized;
    }
    if (lbn >= handle.logical_blocks) {
        return core.err_out_of_range;
    }
    out.* = mapSlice(handle)[lbn];
    return core.ok;
}

// ---------------------------------------------------------------------------
// Checkpoint persistence (`ra8_ftl_checkpoint.c`).
//
// Note the DIFFERENT log tag: the checkpoint translation unit logs under
// "ra8_ftl_checkpoint", not "ra8_ftl", and every message below is verbatim.
// ---------------------------------------------------------------------------

const checkpoint_tag: [*:0]const u8 = "ra8_ftl_checkpoint";

fn scratchSlice(ftl: *const Ftl) []u8 {
    return ftl.scratch.?[0..core.ck_scratch_bytes];
}

/// `internal_ready`: all four caller-owned spans must be bound.
fn checkpointReady(ftl: *const Ftl) Err {
    if (ftl.raw == null) {
        return core.err_not_initialized;
    }
    if (ftl.map == null) {
        return core.err_not_initialized;
    }
    if (ftl.pblocks == null) {
        return core.err_not_initialized;
    }
    return if (ftl.scratch == null) core.err_not_initialized else core.ok;
}

/// `internal_disjoint`: scratch against both live tables (an FTL fault), then
/// the caller's checkpoint span against all three (a caller fault).
///
/// The two byte counts use the C's wrapping `uint32_t` arithmetic; both are
/// bounded anyway because the geometry was sized before this runs.
fn checkpointDisjoint(ftl: *const Ftl, buffer: usize, buffer_bytes: u32) Err {
    const map_bytes = ftl.logical_blocks *% @as(u32, @sizeOf(u16));
    const pb_bytes = ftl.physical_blocks *% @as(u32, @sizeOf(core.Pblock));
    const scratch = @intFromPtr(ftl.scratch.?);
    const map = @intFromPtr(ftl.map.?);
    const pblocks = @intFromPtr(ftl.pblocks.?);

    if (core.rangesOverlap(scratch, core.ck_scratch_bytes, map, map_bytes)) {
        return core.err_invalid_state;
    }
    if (core.rangesOverlap(scratch, core.ck_scratch_bytes, pblocks, pb_bytes)) {
        return core.err_invalid_state;
    }
    if (core.rangesOverlap(buffer, buffer_bytes, map, map_bytes)) {
        return core.err_invalid_arg;
    }
    if (core.rangesOverlap(buffer, buffer_bytes, pblocks, pb_bytes)) {
        return core.err_invalid_arg;
    }
    return if (core.rangesOverlap(buffer, buffer_bytes, scratch, core.ck_scratch_bytes))
        core.err_invalid_arg
    else
        core.ok;
}

pub export fn ra8_ftl_checkpoint_size(ftl: ?*const Ftl, size_out: ?*u32) callconv(.c) Err {
    const handle = ftl orelse {
        ra8_log_emit_error(checkpoint_tag, "ftl must not be nullptr");
        return core.err_null_ptr;
    };
    const out = size_out orelse {
        ra8_log_emit_error(checkpoint_tag, "size_out must not be nullptr");
        return core.err_null_ptr;
    };
    const ready = checkpointReady(handle);
    if (ready != core.ok) {
        return ready;
    }
    switch (core.sizeValues(handle.logical_blocks, handle.physical_blocks)) {
        .fault => |f| return f,
        .value => |v| {
            out.* = v;
            return core.ok;
        },
    }
}

pub export fn ra8_ftl_checkpoint_save(ftl: ?*const Ftl, buf: ?[*]u8, buf_len: u32) callconv(.c) Err {
    const handle = ftl orelse {
        ra8_log_emit_error(checkpoint_tag, "ftl must not be nullptr");
        return core.err_null_ptr;
    };
    const dst = buf orelse {
        ra8_log_emit_error(checkpoint_tag, "buf must not be nullptr");
        return core.err_null_ptr;
    };
    // The C calls the public sizing entry point, so an unbound handle answers
    // not_initialized here and nothing is logged a second time.
    var need: u32 = 0;
    const sized = ra8_ftl_checkpoint_size(handle, &need);
    if (sized != core.ok) {
        return sized;
    }
    if (buf_len < need) {
        return core.err_invalid_size;
    }
    // Save judges aliasing over the bytes it will WRITE, which is `need`.
    const disjoint = checkpointDisjoint(handle, @intFromPtr(dst), need);
    if (disjoint != core.ok) {
        return disjoint;
    }
    const valid = core.validateNative(mapSlice(handle), pblockSlice(handle), scratchSlice(handle));
    if (valid != core.ok) {
        return valid;
    }
    core.encode(mapSlice(handle), pblockSlice(handle), dst[0..need], need);
    return core.ok;
}

pub export fn ra8_ftl_checkpoint_load(ftl: ?*Ftl, buf: ?[*]const u8, buf_len: u32) callconv(.c) Err {
    const handle = ftl orelse {
        ra8_log_emit_error(checkpoint_tag, "ftl must not be nullptr");
        return core.err_null_ptr;
    };
    const src = buf orelse {
        ra8_log_emit_error(checkpoint_tag, "buf must not be nullptr");
        return core.err_null_ptr;
    };
    var need: u32 = 0;
    const sized = ra8_ftl_checkpoint_size(handle, &need);
    if (sized != core.ok) {
        return sized;
    }
    // Load judges aliasing over the bytes it will READ, which is the caller's
    // `buf_len`, before the header has proven that length is the right one.
    const disjoint = checkpointDisjoint(handle, @intFromPtr(src), buf_len);
    if (disjoint != core.ok) {
        return disjoint;
    }
    const bytes = src[0..buf_len];
    const header = core.validateHeader(
        bytes,
        buf_len,
        need,
        handle.logical_blocks,
        handle.physical_blocks,
    );
    if (header != core.ok) {
        return header;
    }
    const valid = core.validateWire(
        bytes,
        handle.logical_blocks,
        handle.physical_blocks,
        scratchSlice(handle),
    );
    if (valid != core.ok) {
        return valid;
    }
    // Transactional: both live tables are touched only now.
    core.decodeCommit(mapSlice(handle), pblockSlice(handle), bytes);
    return core.ok;
}
