//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI of inc/ra8_io_blockdev_mram.h (RA8FW-726): a block device over a
//! window of the RA8D2 extra-MRAM region. Reads are memory-mapped copies;
//! writes and erases go through ra8_flash_extra_mram_write/erase in 32-byte
//! units. Replaces ra8_io_blockdev_mram.c, which is deleted.

const sdhi = @import("ra8_io_blockdev_sdhi_abi.zig");

const tag = "ra8_io_blockdev_mram";

/// ra8_err_t values this unit returns (ra8_err.h).
pub const ok = sdhi.ok;
pub const err_invalid_arg: c_int = 0x103;
pub const err_not_supported: c_int = 0x107;
pub const err_out_of_range: c_int = 0x208;
pub const err_null_ptr = sdhi.err_null_ptr;

pub const Caps = sdhi.Caps;
pub const Iface = sdhi.Iface;
pub const Device = sdhi.Device;

/// k_ra8_mram_write_size_bytes and k_ra8_mram_block_size_bytes (ra8_flash_regs.h).
pub const program_bytes: u32 = 32;
pub const erase_block_bytes: u32 = 32;
/// k_ra8_flash_extra_start / k_ra8_flash_extra_size (ra8_flash_regs.h).
pub const extra_start: usize = 0x02E0_7600;
pub const extra_size: usize = 0x0001_0400;
/// k_ra8_io_erase_value_ones (ra8_io_blockdev.h).
pub const erase_value_ones: u8 = 0xFF;
const block_bytes: usize = sdhi.block_bytes;

/// Mirror of ra8_io_blockdev_mram_state_t.
pub const State = extern struct {
    base: usize,
    block_count: u32,
    read_only: bool,
};

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;
extern fn ra8_flash_extra_mram_write(mram_addr: u32, src: [*]const u8, len: u32) c_int;
extern fn ra8_flash_extra_mram_erase(mram_addr: u32) c_int;

fn nullPtr(message: [*:0]const u8) c_int {
    ra8_log_emit_error(tag, message);
    return err_null_ptr;
}

fn bounds(st: *const State, lba: u32, count: u32) c_int {
    if (count > st.block_count) return err_out_of_range;
    if (lba > st.block_count - count) return err_out_of_range;
    return ok;
}

/// The window must be erase-block aligned and lie inside the extra region.
pub fn windowOk(base: usize, block_count: u32) c_int {
    const extra_end = extra_start + extra_size;
    const span = @as(usize, block_count) * block_bytes;
    if (base % erase_block_bytes != 0) return err_invalid_arg;
    if (span % erase_block_bytes != 0) return err_invalid_arg;
    if (base < extra_start or base >= extra_end) return err_invalid_arg;
    if (span > extra_end - base) return err_invalid_arg;
    return ok;
}

fn state(ctx: ?*anyopaque) ?*const State {
    return @ptrCast(@alignCast(ctx orelse return null));
}

fn read(ctx: ?*anyopaque, lba: u32, count: u32, buf: ?[*]u8) callconv(.c) c_int {
    const st = state(ctx) orelse return nullPtr("ctx must not be nullptr");
    const dst = buf orelse return nullPtr("buf must not be nullptr");
    const b = bounds(st, lba, count);
    if (b != ok) return b;
    const n = @as(usize, count) * block_bytes;
    const src: [*]const u8 = @ptrFromInt(st.base + @as(usize, lba) * block_bytes);
    @memcpy(dst[0..n], src[0..n]);
    return ok;
}

fn writable(st: *const State, lba: u32, count: u32) c_int {
    if (st.read_only) return err_not_supported;
    return bounds(st, lba, count);
}

fn write(ctx: ?*anyopaque, lba: u32, count: u32, buf: ?[*]const u8) callconv(.c) c_int {
    const st = state(ctx) orelse return nullPtr("ctx must not be nullptr");
    const src = buf orelse return nullPtr("buf must not be nullptr");
    const b = writable(st, lba, count);
    if (b != ok) return b;
    const off = @as(usize, lba) * block_bytes;
    const span = @as(usize, count) * block_bytes;
    var done: usize = 0;
    while (done < span) : (done += program_bytes) {
        const addr: u32 = @truncate(st.base + off + done);
        const rc = ra8_flash_extra_mram_write(addr, src + done, program_bytes);
        if (rc != ok) return rc;
    }
    return ok;
}

fn erase(ctx: ?*anyopaque, lba: u32, count: u32) callconv(.c) c_int {
    const st = state(ctx) orelse return nullPtr("ctx must not be nullptr");
    const b = writable(st, lba, count);
    if (b != ok) return b;
    const off = @as(usize, lba) * block_bytes;
    const span = @as(usize, count) * block_bytes;
    var done: usize = 0;
    while (done < span) : (done += erase_block_bytes) {
        const rc = ra8_flash_extra_mram_erase(@truncate(st.base + off + done));
        if (rc != ok) return rc;
    }
    return ok;
}

fn getCaps(ctx: ?*const anyopaque, out: ?*Caps) callconv(.c) c_int {
    const raw = ctx orelse return nullPtr("ctx must not be nullptr");
    const caps = out orelse return nullPtr("out must not be nullptr");
    const st: *const State = @ptrCast(@alignCast(raw));
    caps.* = .{
        .block_count = st.block_count,
        .erase_unit_blocks = 1,
        .program_size_bytes = program_bytes,
        .logical_block_bytes = sdhi.block_bytes,
        .erase_value = erase_value_ones,
        .must_erase_before_write = true,
        .read_only = st.read_only,
    };
    return ok;
}

/// The MRAM vtable; sync is null because each program commits inline.
pub const iface: Iface = .{
    .read = read,
    .write = write,
    .erase = erase,
    .get_caps = getCaps,
    .sync = null,
};

pub export fn ra8_io_blockdev_mram_init(
    bd: ?*Device,
    st: ?*State,
    base_addr: usize,
    block_count: u32,
    read_only: bool,
) c_int {
    const device = bd orelse return nullPtr("bd must not be nullptr");
    const s = st orelse return nullPtr("state must not be nullptr");
    if (block_count < 1) return err_invalid_arg;
    const w = windowOk(base_addr, block_count);
    if (w != ok) return w;
    s.* = .{ .base = base_addr, .block_count = block_count, .read_only = read_only };
    device.* = .{ .iface = &iface, .ctx = s };
    return ok;
}
