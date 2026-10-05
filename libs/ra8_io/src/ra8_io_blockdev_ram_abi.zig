//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI of inc/ra8_io_blockdev_ram.h (RA8FW-722): a caller-owned RAM buffer
//! as a block device. Read, write and erase are bounds-checked memcpy and
//! memset over 512-byte blocks; a read-only device rejects write and erase.
//! There is no write buffering, so sync stays null. Replaces
//! ra8_io_blockdev_ram.c, which is deleted.

const sdhi = @import("ra8_io_blockdev_sdhi_abi.zig");

const tag = "ra8_io_blockdev_ram";

/// ra8_err_t values this unit returns (ra8_err.h).
pub const ok = sdhi.ok;
pub const err_invalid_size: c_int = 0x105;
pub const err_not_supported: c_int = 0x107;
pub const err_out_of_range: c_int = 0x208;
pub const err_null_ptr = sdhi.err_null_ptr;

pub const Caps = sdhi.Caps;
pub const Iface = sdhi.Iface;
pub const Device = sdhi.Device;

pub const block_bytes: usize = sdhi.block_bytes;
/// Smallest legal device size, in blocks.
pub const min_blocks: u32 = 1;

/// Mirror of ra8_io_blockdev_ram_state_t.
pub const State = extern struct {
    storage: ?[*]u8,
    block_count: u32,
    read_only: bool,
};

comptime {
    if (@offsetOf(State, "block_count") != @sizeOf(usize)) @compileError("ram state layout drifted");
    if (@offsetOf(State, "read_only") != @sizeOf(usize) + 4) @compileError("ram state layout drifted");
}

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;

fn nullPtr(message: [*:0]const u8) c_int {
    ra8_log_emit_error(tag, message);
    return err_null_ptr;
}

fn bounds(st: *const State, lba: u32, count: u32) c_int {
    if (count > st.block_count) return err_out_of_range;
    if (lba > st.block_count - count) return err_out_of_range;
    return ok;
}

fn span(st: *const State, lba: u32, count: u32) []u8 {
    const off = @as(usize, lba) * block_bytes;
    const n = @as(usize, count) * block_bytes;
    return st.storage.?[off .. off + n];
}

fn read(ctx: ?*anyopaque, lba: u32, count: u32, buf: ?[*]u8) callconv(.c) c_int {
    const raw = ctx orelse return nullPtr("ctx must not be nullptr");
    const dst = buf orelse return nullPtr("buf must not be nullptr");
    const st: *const State = @ptrCast(@alignCast(raw));
    const b = bounds(st, lba, count);
    if (b != ok) return b;
    const src = span(st, lba, count);
    @memcpy(dst[0..src.len], src);
    return ok;
}

fn write(ctx: ?*anyopaque, lba: u32, count: u32, buf: ?[*]const u8) callconv(.c) c_int {
    const raw = ctx orelse return nullPtr("ctx must not be nullptr");
    const src = buf orelse return nullPtr("buf must not be nullptr");
    const st: *const State = @ptrCast(@alignCast(raw));
    if (st.read_only) return err_not_supported;
    const b = bounds(st, lba, count);
    if (b != ok) return b;
    const dst = span(st, lba, count);
    @memcpy(dst, src[0..dst.len]);
    return ok;
}

fn erase(ctx: ?*anyopaque, lba: u32, count: u32) callconv(.c) c_int {
    const raw = ctx orelse return nullPtr("ctx must not be nullptr");
    const st: *const State = @ptrCast(@alignCast(raw));
    if (st.read_only) return err_not_supported;
    const b = bounds(st, lba, count);
    if (b != ok) return b;
    @memset(span(st, lba, count), sdhi.erase_value_zero);
    return ok;
}

fn getCaps(ctx: ?*const anyopaque, out: ?*Caps) callconv(.c) c_int {
    const raw = ctx orelse return nullPtr("ctx must not be nullptr");
    const caps = out orelse return nullPtr("out must not be nullptr");
    const st: *const State = @ptrCast(@alignCast(raw));
    caps.* = .{
        .block_count = st.block_count,
        .erase_unit_blocks = sdhi.erase_unit_blocks,
        .program_size_bytes = sdhi.block_bytes,
        .logical_block_bytes = sdhi.block_bytes,
        .erase_value = sdhi.erase_value_zero,
        .must_erase_before_write = false,
        .read_only = st.read_only,
    };
    return ok;
}

pub const iface: Iface = .{
    .read = read,
    .write = write,
    .erase = erase,
    .get_caps = getCaps,
    .sync = null,
};

export fn ra8_io_blockdev_ram_init(
    bd: ?*Device,
    state: ?*State,
    storage: ?[*]u8,
    block_count: u32,
    read_only: bool,
) c_int {
    const device = bd orelse return nullPtr("bd must not be nullptr");
    const st = state orelse return nullPtr("state must not be nullptr");
    const bytes = storage orelse return nullPtr("storage must not be nullptr");
    if (block_count < min_blocks) return err_invalid_size;
    st.* = .{ .storage = bytes, .block_count = block_count, .read_only = read_only };
    device.* = .{ .iface = &iface, .ctx = st };
    return ok;
}
