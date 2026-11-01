//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The ra8_io netif front end (RA8FW-318): validation and dispatch through
//! a bound backend vtable, against a fake backend.

const std = @import("std");
const io = @import("ra8_io");
const front = io.netif;
const Bus = front.Bus;
const Iface = front.Iface;

const err_invalid_arg: c_int = 0x103;
const err_no_data: c_int = 0x10A;

var errors_logged: u32 = 0;
var calls: u32 = 0;
var last_ctx: ?*anyopaque = null;
var last_frame: ?[*]const u8 = null;
var last_len: u16 = 0;
var last_out: ?[*]u8 = null;
var last_inout: ?*u16 = null;
var result: c_int = 0;

export fn ra8_log_emit_error(_: [*:0]const u8, _: [*:0]const u8) void {
    errors_logged += 1;
}

// The other units in the same archive need these to link; unused here.
export fn ra8_log_set_byte_sink(_: ?io.log.ByteSink, _: ?*anyopaque) void {}
export fn ra8_log_emit_error_val(_: [*:0]const u8, _: [*:0]const u8, _: u32) void {}
export fn ra8_sci_write_polling(_: u8, _: [*]const u8, _: u32) c_int {
    return 0;
}
export fn ra8_sci_flush(_: u8) c_int {
    return 0;
}
export fn ra8_usb_pal_ep_send(_: u8, _: [*]const u8, _: u16) c_int {
    return 0;
}
export fn ra8_sdramc_init() c_int {
    return 0;
}
export fn ra8_sci_spi_xfer8(_: u8, _: u8, _: ?*u8) c_int {
    return 0;
}
export fn ra8_sci_spi_xfer(_: u8, _: ?[*]const u8, _: ?[*]u8, _: u32) c_int {
    return 0;
}
export fn ra8_sci_spi_set_clock(_: u8, _: u32, _: u32) c_int {
    return 0;
}
export fn ra8_usb_hmsc_write10(_: u8, _: u32, _: ?[*]const u8, _: u16) c_int {
    return 0;
}
export fn ra8_usb_hmsc_read10(_: u8, _: u32, _: ?[*]u8, _: u16) c_int {
    return 0;
}
export fn ra8_i3c_write(_: u8, _: ?[*]const u8, _: u32, _: bool) c_int {
    return 0;
}
export fn ra8_i3c_read(_: u8, _: ?[*]u8, _: u32, _: bool) c_int {
    return 0;
}
export fn ra8_i3c_transfer(_: u8, _: ?*const anyopaque, _: u32, _: ?*const anyopaque, _: u32) c_int {
    return 0;
}
// Externs the other archive units reference; no-ops here (unused).
export fn ra8_flash_extra_mram_erase(mram_addr: u32) c_int {
    _ = mram_addr;
    return 0;
}

export fn ra8_flash_extra_mram_write(mram_addr: u32, src: [*]const u8, len: u32) c_int {
    _ = mram_addr;
    _ = src;
    _ = len;
    return 0;
}

export fn ra8_fs_close(file: ?*anyopaque) u16 {
    _ = file;
    return 0;
}

export fn ra8_fs_dir_close(dir: *anyopaque) u16 {
    _ = dir;
    return 0;
}

export fn ra8_fs_dir_next(dir: *anyopaque, out: *anyopaque, out_entry: *bool) u16 {
    _ = dir;
    _ = out;
    _ = out_entry;
    return 0;
}

export fn ra8_fs_dir_open(mount: ?*anyopaque, path: [*:0]const u8, dir: *anyopaque) u16 {
    _ = mount;
    _ = path;
    _ = dir;
    return 0;
}

export fn ra8_fs_free_space(mount: ?*anyopaque, out: *anyopaque) u16 {
    _ = mount;
    _ = out;
    return 0;
}

export fn ra8_fs_mkdir(mount: ?*anyopaque, path: [*:0]const u8) u16 {
    _ = mount;
    _ = path;
    return 0;
}

export fn ra8_fs_mount(fs_mount_backend: *const anyopaque, out: *?*anyopaque) u16 {
    _ = fs_mount_backend;
    _ = out;
    return 0;
}

export fn ra8_fs_open(mount: ?*anyopaque, path: [*:0]const u8, mode: c_int, out: *?*anyopaque) u16 {
    _ = mount;
    _ = path;
    _ = mode;
    _ = out;
    return 0;
}

export fn ra8_fs_probe(fs_probe_backend: *const anyopaque, out_type: *c_int) u16 {
    _ = fs_probe_backend;
    _ = out_type;
    return 0;
}

export fn ra8_fs_read(file: ?*anyopaque, buf: *anyopaque, bytes: u32, out_read: *u32) u16 {
    _ = file;
    _ = buf;
    _ = bytes;
    _ = out_read;
    return 0;
}

export fn ra8_fs_rename(mount: ?*anyopaque, old_path: [*:0]const u8, new_path: [*:0]const u8) u16 {
    _ = mount;
    _ = old_path;
    _ = new_path;
    return 0;
}

export fn ra8_fs_rmdir(mount: ?*anyopaque, path: [*:0]const u8) u16 {
    _ = mount;
    _ = path;
    return 0;
}

export fn ra8_fs_seek(file: ?*anyopaque, offset: u64) u16 {
    _ = file;
    _ = offset;
    return 0;
}

export fn ra8_fs_size(file: ?*const anyopaque, out: *u64) u16 {
    _ = file;
    _ = out;
    return 0;
}

export fn ra8_fs_stat(mount: ?*anyopaque, path: [*:0]const u8, out: *anyopaque) u16 {
    _ = mount;
    _ = path;
    _ = out;
    return 0;
}

export fn ra8_fs_tell(file: ?*const anyopaque, out: *u64) u16 {
    _ = file;
    _ = out;
    return 0;
}

export fn ra8_fs_unlink(mount: ?*anyopaque, path: [*:0]const u8) u16 {
    _ = mount;
    _ = path;
    return 0;
}

export fn ra8_fs_unmount(mount: ?*anyopaque) u16 {
    _ = mount;
    return 0;
}

export fn ra8_fs_write(file: ?*anyopaque, buf: *const anyopaque, bytes: u32) u16 {
    _ = file;
    _ = buf;
    _ = bytes;
    return 0;
}

export fn ra8_sdcard_get_capacity(out_blocks: *u32) c_int {
    _ = out_blocks;
    return 0;
}

export fn ra8_sdcard_read_blocks(lba: u32, buf: [*]u8, count: u32) c_int {
    _ = lba;
    _ = buf;
    _ = count;
    return 0;
}

export fn ra8_sdcard_write_blocks(lba: u32, buf: [*]const u8, count: u32) c_int {
    _ = lba;
    _ = buf;
    _ = count;
    return 0;
}

export fn ra8_sdmmc_spi_erase_blocks(lba: u32, count: u32) c_int {
    _ = lba;
    _ = count;
    return 0;
}

export fn ra8_sdmmc_spi_get_capacity(out_blocks: *u32) c_int {
    _ = out_blocks;
    return 0;
}

export fn ra8_sdmmc_spi_read_blocks(lba: u32, buf: [*]u8, count: u32) c_int {
    _ = lba;
    _ = buf;
    _ = count;
    return 0;
}

export fn ra8_sdmmc_spi_write_blocks(lba: u32, buf: [*]const u8, count: u32) c_int {
    _ = lba;
    _ = buf;
    _ = count;
    return 0;
}

export fn ra8_usb_hmsc_read_capacity(lun: u8, block_count: *u32, block_size: *u32) c_int {
    _ = lun;
    _ = block_count;
    _ = block_size;
    return 0;
}

export fn ra8_i2c_write(_: u8, _: u16, _: ?[*]const u8, _: u32, _: bool) c_int {
    return 0;
}
export fn ra8_i2c_read(_: u8, _: u16, _: ?[*]u8, _: u32, _: bool) c_int {
    return 0;
}
export fn ra8_i2c_transfer(_: u8, _: ?*const anyopaque, _: u32, _: ?*const anyopaque, _: u32) c_int {
    return 0;
}
export fn ra8_spi_xfer8(_: u8, _: u8, _: ?*u8) c_int {
    return 0;
}
export fn ra8_spi_write_read(_: u8, _: ?*const anyopaque, _: ?*anyopaque, _: u32, _: u8) c_int {
    return 0;
}
export fn ra8_spi_set_clock(_: u8, _: u32, _: u32) c_int {
    return 0;
}
export fn ra8_net_pal_send_frame(_: ?[*]const u8, _: u16) u16 {
    return 0;
}
export fn ra8_net_pal_recv_frame(_: ?[*]u8, _: ?*u16) u16 {
    return 0;
}
export fn ra8_c6link_eth_send(_: ?*anyopaque, _: ?[*]const u8, _: u16) c_int {
    return 0;
}
export fn ra8_c6link_poll(_: ?*anyopaque, _: u16, _: ?*anyopaque) c_int {
    return 0;
}

var fake_ctx: u8 = 7;

fn fakeSend(ctx: ?*anyopaque, fbuf: ?[*]const u8, len: u16) callconv(.c) c_int {
    calls += 1;
    last_ctx = ctx;
    last_frame = fbuf;
    last_len = len;
    return result;
}

fn fakePoll(ctx: ?*anyopaque, out: ?[*]u8, inout: ?*u16) callconv(.c) c_int {
    calls += 1;
    last_ctx = ctx;
    last_out = out;
    last_inout = inout;
    return result;
}

const fake_iface = Iface{ .send = fakeSend, .poll = fakePoll };
const no_send_iface = Iface{ .send = null, .poll = fakePoll };
const no_poll_iface = Iface{ .send = fakeSend, .poll = null };

extern fn ra8_io_netif_send(netif: ?*const Bus, frame: ?[*]const u8, len: u16) c_int;
extern fn ra8_io_netif_poll(netif: ?*const Bus, out_buf: ?[*]u8, inout_len: ?*u16) c_int;

const frame = [_]u8{ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x11 };

test "send: NULL handle is refused" {
    errors_logged = 0;
    try std.testing.expectEqual(front.err_null_ptr, ra8_io_netif_send(null, &frame, frame.len));
    try std.testing.expectEqual(@as(u32, 0), errors_logged);
}

test "send: unbound handle is refused" {
    var netif: Bus = .{ .iface = null, .ctx = null };
    try std.testing.expectEqual(front.err_not_initialized, ra8_io_netif_send(&netif, &frame, frame.len));
}

test "send: dispatches with the backend context" {
    var netif: Bus = .{ .iface = &fake_iface, .ctx = &fake_ctx };
    calls = 0;
    result = 0;
    last_frame = null;
    last_len = 0;
    try std.testing.expectEqual(front.ok, ra8_io_netif_send(&netif, &frame, frame.len));
    try std.testing.expectEqual(@as(u32, 1), calls);
    try std.testing.expectEqual(@as(?*anyopaque, &fake_ctx), last_ctx);
    try std.testing.expectEqual(@as(?[*]const u8, &frame), last_frame);
    try std.testing.expectEqual(@as(u16, frame.len), last_len);
}

test "send: the transport's verdict passes through" {
    var netif: Bus = .{ .iface = &fake_iface, .ctx = &fake_ctx };
    calls = 0;
    result = err_no_data;
    try std.testing.expectEqual(err_no_data, ra8_io_netif_send(&netif, &frame, frame.len));
}

test "send: a malformed backend is reported once" {
    var netif: Bus = .{ .iface = &no_send_iface, .ctx = &fake_ctx };
    errors_logged = 0;
    try std.testing.expectEqual(front.err_null_ptr, ra8_io_netif_send(&netif, &frame, frame.len));
    try std.testing.expectEqual(@as(u32, 1), errors_logged);
}

test "poll: dispatches with the backend context and passes the verdict through" {
    var netif: Bus = .{ .iface = &fake_iface, .ctx = &fake_ctx };
    var buf: [16]u8 = undefined;
    var len: u16 = @sizeOf(@TypeOf(buf));
    calls = 0;
    result = err_no_data;
    last_out = null;
    last_inout = null;
    try std.testing.expectEqual(err_no_data, ra8_io_netif_poll(&netif, &buf, &len));
    try std.testing.expectEqual(@as(u32, 1), calls);
    try std.testing.expectEqual(@as(?*anyopaque, &fake_ctx), last_ctx);
    try std.testing.expectEqual(@as(?[*]u8, &buf), last_out);
    try std.testing.expectEqual(@as(?*u16, &len), last_inout);
}

test "poll: a malformed backend is reported once" {
    var netif: Bus = .{ .iface = &no_poll_iface, .ctx = &fake_ctx };
    var len: u16 = 16;
    errors_logged = 0;
    try std.testing.expectEqual(front.err_null_ptr, ra8_io_netif_poll(&netif, null, &len));
    try std.testing.expectEqual(@as(u32, 1), errors_logged);
}

test "poll: NULL handle is refused" {
    try std.testing.expectEqual(front.err_null_ptr, ra8_io_netif_poll(null, null, null));
}

test "invalid_arg shares the generic range" {
    try std.testing.expectEqual(err_invalid_arg, @as(c_int, 0x103));
}
