//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI of inc/ra8_io_blockdev_vsource.h (RA8FW-1083): binds a block device
//! into a caller-owned one-sector bounce-buffer context and services a
//! byte-offset read over it as an unaligned head, a whole-sector middle and an
//! unaligned tail. Replaces ra8_io_blockdev_vsource.c, which is deleted.

const std = @import("std");
const blockdev = @import("ra8_io_blockdev_abi.zig");

const tag = "ra8_io_blockdev_vsource";

/// ra8_err_t values this unit returns (ra8_err.h).
pub const ok = blockdev.ok;
pub const err_out_of_range: c_int = 0x208;
pub const err_null_ptr = blockdev.err_null_ptr;

pub const Device = blockdev.Device;

/// Mirror of k_ra8_io_block_size_bytes (ra8_io_blockdev.h).
pub const block_bytes: usize = 512;

/// Mirror of ra8_io_blockdev_vsource_ctx_t: the bound device plus the
/// one-sector bounce buffer.
pub const Ctx = extern struct {
    bd: ?*const Device,
    scratch: [block_bytes]u8,
};

comptime {
    if (@offsetOf(Ctx, "scratch") != @sizeOf(usize)) @compileError("vsource ctx layout drifted");
    if (@sizeOf(Ctx) != @sizeOf(usize) + block_bytes) @compileError("vsource ctx layout drifted");
}

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;
extern fn ra8_io_blockdev_read(bd: ?*const Device, lba: u32, count: u32, buf: ?[*]u8) c_int;

fn nullPtr(message: [*:0]const u8) c_int {
    ra8_log_emit_error(tag, message);
    return err_null_ptr;
}

/// Mutable walk state shared by the head/middle/tail read segments: the
/// absolute device byte offset of the next unread byte, the next unwritten
/// byte of the caller buffer, and the bytes still owed to the caller.
const Cursor = struct {
    cur: u64,
    out: [*]u8,
    remain: u32,
};

/// Read one sector at `lba` into the context bounce buffer.
fn readSector(ctx: *Ctx, lba: u32) c_int {
    return ra8_io_blockdev_read(ctx.bd, lba, 1, &ctx.scratch);
}

/// Read the sector at `lba` into the bounce buffer, then copy `n` bytes
/// starting at `in_block` of that sector into `dst`.
fn copySlice(ctx: *Ctx, lba: u32, in_block: u32, n: u32, dst: [*]u8) c_int {
    const err = readSector(ctx, lba);
    if (err != ok) return err;
    const from = ctx.scratch[in_block .. in_block + n];
    @memcpy(dst[0..n], from);
    return ok;
}

/// Service the unaligned head sector: when the request does not start on a
/// sector boundary, copy the partial leading sector through the bounce buffer
/// and advance the cursor past it, clamped so a sub-sector request never
/// copies past `cur.remain`.
fn readHead(ctx: *Ctx, cur: *Cursor) c_int {
    const head_in_block: u32 = @intCast(cur.cur % block_bytes);
    if (head_in_block == 0) return ok;
    var head_n: u32 = @intCast(block_bytes - @as(u64, head_in_block));
    if (head_n > cur.remain) head_n = cur.remain;
    const err = copySlice(ctx, @truncate(cur.cur / block_bytes), head_in_block, head_n, cur.out);
    if (err != ok) return err;
    cur.cur += head_n;
    cur.out += head_n;
    cur.remain -= head_n;
    return ok;
}

/// Service the aligned middle: read every whole sector that fits in the
/// remaining length straight into the caller buffer with no intermediate
/// copy, then advance the cursor past them.
fn readMiddle(ctx: *Ctx, cur: *Cursor) c_int {
    const mid_blocks: u32 = @intCast(@as(u64, cur.remain) / block_bytes);
    if (mid_blocks == 0) return ok;
    const err = ra8_io_blockdev_read(ctx.bd, @truncate(cur.cur / block_bytes), mid_blocks, cur.out);
    if (err != ok) return err;
    const mid_bytes: u32 = @intCast(@as(u64, mid_blocks) * block_bytes);
    cur.cur += mid_bytes;
    cur.out += mid_bytes;
    cur.remain -= mid_bytes;
    return ok;
}

/// Service the unaligned tail: copy any sub-sector remainder left after the
/// aligned middle through the bounce buffer. The tail always starts on a
/// sector boundary, so the in-block offset is zero.
fn readTail(ctx: *Ctx, cur: *Cursor) c_int {
    if (cur.remain == 0) return ok;
    return copySlice(ctx, @truncate(cur.cur / block_bytes), 0, cur.remain, cur.out);
}

export fn ra8_io_blockdev_vsource_init(ctx: ?*Ctx, bd: ?*const Device) c_int {
    if (ctx == null) return nullPtr("ctx must not be nullptr");
    if (bd == null) return nullPtr("bd must not be nullptr");
    ctx.?.bd = bd;
    @memset(&ctx.?.scratch, 0);
    return ok;
}

export fn ra8_io_blockdev_vsource_read(ctx_v: ?*anyopaque, offset: u64, buf: ?[*]u8, len: u32) c_int {
    if (ctx_v == null) return nullPtr("ctx must not be nullptr");
    const dst = buf orelse return nullPtr("buf must not be nullptr");
    const ctx: *Ctx = @ptrCast(@alignCast(ctx_v.?));
    if (offset > std.math.maxInt(u64) - @as(u64, len)) return err_out_of_range;

    var cur = Cursor{ .cur = offset, .out = dst, .remain = len };
    const head_err = readHead(ctx, &cur);
    if (head_err != ok) return head_err;
    const mid_err = readMiddle(ctx, &cur);
    if (mid_err != ok) return mid_err;
    return readTail(ctx, &cur);
}
