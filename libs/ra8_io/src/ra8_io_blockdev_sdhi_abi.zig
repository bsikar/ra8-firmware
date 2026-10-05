//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI of inc/ra8_io_blockdev_sdhi.h (RA8FW-715): the SD card behind the
//! SDHI driver as a block device. The vtable forwards read, write and
//! capacity to ra8_sdcard_*; there is no erase and no write buffering, so
//! both slots stay null. Replaces ra8_io_blockdev_sdhi.c, which is deleted.

const tag = "ra8_io_blockdev_sdhi";

/// ra8_err_t values this unit returns (ra8_err.h).
pub const ok: c_int = 0;
pub const err_null_ptr: c_int = 0x504;

/// ra8_io_blockdev.h: k_ra8_io_block_size_bytes, k_ra8_io_erase_value_zero.
pub const block_bytes: u16 = 512;
pub const erase_value_zero: u8 = 0x00;
/// One logical block per erase unit.
pub const erase_unit_blocks: u32 = 1;

/// Mirror of ra8_io_blockdev_caps_t.
pub const Caps = extern struct {
    block_count: u32,
    erase_unit_blocks: u32,
    program_size_bytes: u32,
    logical_block_bytes: u16,
    erase_value: u8,
    must_erase_before_write: bool,
    read_only: bool,
};

/// Mirror of struct ra8_io_blockdev_iface (ra8_io_blockdev_backend.h).
pub const Iface = extern struct {
    read: ?*const fn (?*anyopaque, u32, u32, ?[*]u8) callconv(.c) c_int,
    write: ?*const fn (?*anyopaque, u32, u32, ?[*]const u8) callconv(.c) c_int,
    erase: ?*const fn (?*anyopaque, u32, u32) callconv(.c) c_int,
    get_caps: ?*const fn (?*const anyopaque, ?*Caps) callconv(.c) c_int,
    sync: ?*const fn (?*anyopaque) callconv(.c) c_int,
};

/// Mirror of ra8_io_blockdev_t.
pub const Device = extern struct {
    iface: ?*const Iface,
    ctx: ?*anyopaque,
};

comptime {
    if (@sizeOf(Caps) != 20) @compileError("ra8_io_blockdev_caps_t is 20 bytes");
    if (@offsetOf(Caps, "logical_block_bytes") != 12) @compileError("caps layout drifted");
    if (@offsetOf(Caps, "read_only") != 16) @compileError("caps layout drifted");
    if (@sizeOf(Iface) != 5 * @sizeOf(usize)) @compileError("iface is five pointers");
    if (@sizeOf(Device) != 2 * @sizeOf(usize)) @compileError("blockdev is two pointers");
}

extern fn ra8_sdcard_read_blocks(lba: u32, buf: [*]u8, count: u32) c_int;
extern fn ra8_sdcard_write_blocks(lba: u32, buf: [*]const u8, count: u32) c_int;
extern fn ra8_sdcard_get_capacity(out_blocks: *u32) c_int;
extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;

fn read(_: ?*anyopaque, lba: u32, count: u32, buf: ?[*]u8) callconv(.c) c_int {
    const dst = buf orelse {
        ra8_log_emit_error(tag, "buf must not be nullptr");
        return err_null_ptr;
    };
    return ra8_sdcard_read_blocks(lba, dst, count);
}

fn write(_: ?*anyopaque, lba: u32, count: u32, buf: ?[*]const u8) callconv(.c) c_int {
    const src = buf orelse {
        ra8_log_emit_error(tag, "buf must not be nullptr");
        return err_null_ptr;
    };
    return ra8_sdcard_write_blocks(lba, src, count);
}

fn getCaps(_: ?*const anyopaque, out: ?*Caps) callconv(.c) c_int {
    const caps = out orelse {
        ra8_log_emit_error(tag, "out must not be nullptr");
        return err_null_ptr;
    };
    var blocks: u32 = 0;
    const rc = ra8_sdcard_get_capacity(&blocks);
    if (rc != ok) return rc;
    caps.* = .{
        .block_count = blocks,
        .erase_unit_blocks = erase_unit_blocks,
        .program_size_bytes = block_bytes,
        .logical_block_bytes = block_bytes,
        .erase_value = erase_value_zero,
        .must_erase_before_write = false,
        .read_only = false,
    };
    return ok;
}

pub const iface: Iface = .{
    .read = read,
    .write = write,
    .erase = null,
    .get_caps = getCaps,
    .sync = null,
};

export fn ra8_io_blockdev_sdhi_init(bd: ?*Device) c_int {
    const device = bd orelse {
        ra8_log_emit_error(tag, "bd must not be nullptr");
        return err_null_ptr;
    };
    device.* = .{ .iface = &iface, .ctx = null };
    return ok;
}
