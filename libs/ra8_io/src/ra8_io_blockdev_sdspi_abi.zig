//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI of inc/ra8_io_blockdev_sdspi.h (RA8FW-718): an SD card in SPI mode
//! as a block device. The vtable forwards read, write, erase and capacity
//! to ra8_sdmmc_spi_*; there is no write buffering, so sync stays null.
//! The caps/iface/device mirrors are the SDHI unit's (RA8FW-715). Replaces
//! ra8_io_blockdev_sdspi.c, which is deleted.

const sdhi = @import("ra8_io_blockdev_sdhi_abi.zig");

const tag = "ra8_io_blockdev_sdspi";

pub const ok = sdhi.ok;
pub const err_null_ptr = sdhi.err_null_ptr;
pub const Caps = sdhi.Caps;
pub const Iface = sdhi.Iface;
pub const Device = sdhi.Device;

extern fn ra8_sdmmc_spi_read_blocks(lba: u32, buf: [*]u8, count: u32) c_int;
extern fn ra8_sdmmc_spi_write_blocks(lba: u32, buf: [*]const u8, count: u32) c_int;
extern fn ra8_sdmmc_spi_erase_blocks(lba: u32, count: u32) c_int;
extern fn ra8_sdmmc_spi_get_capacity(out_blocks: *u32) c_int;
extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;
extern fn ra8_log_emit_error_val(tag: [*:0]const u8, message: [*:0]const u8, value: u32) void;

fn read(_: ?*anyopaque, lba: u32, count: u32, buf: ?[*]u8) callconv(.c) c_int {
    const dst = buf orelse {
        ra8_log_emit_error(tag, "buf must not be nullptr");
        return err_null_ptr;
    };
    return ra8_sdmmc_spi_read_blocks(lba, dst, count);
}

fn write(_: ?*anyopaque, lba: u32, count: u32, buf: ?[*]const u8) callconv(.c) c_int {
    const src = buf orelse {
        ra8_log_emit_error(tag, "buf must not be nullptr");
        return err_null_ptr;
    };
    return ra8_sdmmc_spi_write_blocks(lba, src, count);
}

fn erase(_: ?*anyopaque, lba: u32, count: u32) callconv(.c) c_int {
    return ra8_sdmmc_spi_erase_blocks(lba, count);
}

fn getCaps(_: ?*const anyopaque, out: ?*Caps) callconv(.c) c_int {
    const caps = out orelse {
        ra8_log_emit_error(tag, "out must not be nullptr");
        return err_null_ptr;
    };
    var blocks: u32 = 0;
    const rc = ra8_sdmmc_spi_get_capacity(&blocks);
    if (rc != ok) {
        ra8_log_emit_error(tag, "sd capacity");
        ra8_log_emit_error_val(tag, "Error", @bitCast(rc));
        return rc;
    }
    caps.* = .{
        .block_count = blocks,
        .erase_unit_blocks = sdhi.erase_unit_blocks,
        .program_size_bytes = sdhi.block_bytes,
        .logical_block_bytes = sdhi.block_bytes,
        .erase_value = sdhi.erase_value_zero,
        .must_erase_before_write = false,
        .read_only = false,
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

export fn ra8_io_blockdev_sdspi_init(bd: ?*Device) c_int {
    const device = bd orelse {
        ra8_log_emit_error(tag, "bd must not be nullptr");
        return err_null_ptr;
    };
    device.* = .{ .iface = &iface, .ctx = null };
    return ok;
}
