//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Dumb link stubs for the driver externs the other ra8_io units reference
//! (RA8FW-1084). A test root that defines none of its own driver stubs pulls
//! this in beside netif_stubs.zig and fs_stubs.zig; a root with behavioural
//! or dumb stubs of its own must not import this file, or the two
//! definitions collide at link time. Every stub reports failure except where
//! a void return leaves nothing to report.

const io = @import("ra8_io");

export fn ra8_flash_extra_mram_erase(_: u32) c_int {
    return 0;
}

export fn ra8_flash_extra_mram_write(_: u32, _: [*]const u8, _: u32) c_int {
    return 0;
}

export fn ra8_i2c_read(_: u8, _: u8, _: ?[*]u8, _: u32) c_int {
    return 0;
}

export fn ra8_i2c_transfer(_: u8, _: u8, _: ?[*]const u8, _: u32, _: ?[*]u8, _: u32) c_int {
    return 0;
}

export fn ra8_i2c_write(_: u8, _: u8, _: ?[*]const u8, _: u32, _: bool) c_int {
    return 0;
}

export fn ra8_i3c_read(_: u8, _: u8, _: ?[*]u8, _: u32, _: bool) c_int {
    return 0;
}

export fn ra8_i3c_transfer(_: u8, _: u8, _: ?[*]const u8, _: u32, _: ?[*]u8, _: u32) c_int {
    return 0;
}

export fn ra8_i3c_write(_: u8, _: u8, _: ?[*]const u8, _: u32, _: bool) c_int {
    return 0;
}

export fn ra8_log_set_byte_sink(_: ?io.log.ByteSink, _: ?*anyopaque) void {}

export fn ra8_sci_flush(_: u8) c_int {
    return 0;
}

export fn ra8_sci_spi_set_clock(_: u8, _: u32, _: u32) c_int {
    return 0;
}

export fn ra8_sci_spi_xfer(_: u8, _: ?[*]const u8, _: ?[*]u8, _: u32) c_int {
    return 0;
}

export fn ra8_sci_spi_xfer8(_: u8, _: u8, _: ?*u8) c_int {
    return 0;
}

export fn ra8_sci_write_polling(_: u8, _: [*]const u8, _: u32) c_int {
    return 0;
}

export fn ra8_sdcard_get_capacity(_: *u32) c_int {
    return 0;
}

export fn ra8_sdcard_read_blocks(_: u32, _: [*]u8, _: u32) c_int {
    return 0;
}

export fn ra8_sdcard_write_blocks(_: u32, _: [*]const u8, _: u32) c_int {
    return 0;
}

export fn ra8_sdmmc_spi_erase_blocks(_: u32, _: u32) c_int {
    return 0;
}

export fn ra8_sdmmc_spi_get_capacity(_: *u32) c_int {
    return 0;
}

export fn ra8_sdmmc_spi_read_blocks(_: u32, _: [*]u8, _: u32) c_int {
    return 0;
}

export fn ra8_sdmmc_spi_write_blocks(_: u32, _: [*]const u8, _: u32) c_int {
    return 0;
}

export fn ra8_sdramc_init() c_int {
    return 0;
}

export fn ra8_spi_set_clock(_: u8, _: u32, _: u32) c_int {
    return 0;
}

export fn ra8_spi_write_read(_: u8, _: ?*const anyopaque, _: ?*anyopaque, _: u32, _: u8) c_int {
    return 0;
}

export fn ra8_spi_xfer8(_: u8, _: u8, _: ?*u8) c_int {
    return 0;
}

export fn ra8_usb_hmsc_read10(_: u8, _: u32, _: u16, _: ?[*]u8) c_int {
    return 0;
}

export fn ra8_usb_hmsc_read_capacity(_: u8, _: *u32, _: *u32) c_int {
    return 0;
}

export fn ra8_usb_hmsc_write10(_: u8, _: u32, _: u16, _: ?[*]const u8) c_int {
    return 0;
}

export fn ra8_usb_pal_ep_send(_: u8, _: [*]const u8, _: u16) c_int {
    return 0;
}
