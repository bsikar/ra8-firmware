//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The ra8_io netif ESP32-C6 backend (RA8FW-318): the bind helper, the rx
//! staging sink, and the pump-then-drain poll, against a fake ra8_c6link.

const std = @import("std");
const io = @import("ra8_io");
const front = io.netif;
const backend = io.netif_c6link;
const Bus = front.Bus;
const Ctx = backend.Ctx;

const err_no_data: c_int = 0x10A;
const err_invalid_size: c_int = 0x105;
const err_invalid_arg: c_int = 0x103;
const err_null_ptr: c_int = 0x504;

var errors_logged: u32 = 0;
var pump_calls: u32 = 0;
var send_calls: u32 = 0;
var send_result: c_int = 0;
var last_send_frame: ?[*]const u8 = null;
var last_send_len: u16 = 0;
var last_send_link: ?*anyopaque = null;
var staged_this_pump: u32 = 0;

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

export fn ra8_c6link_eth_send(link: ?*anyopaque, frame: ?[*]const u8, len: u16) c_int {
    send_calls += 1;
    last_send_link = link;
    last_send_frame = frame;
    last_send_len = len;
    return send_result;
}

export fn ra8_c6link_poll(link: ?*anyopaque, max_transactions: u16, stats: ?*anyopaque) c_int {
    _ = stats;
    _ = max_transactions;
    pump_calls += 1;
    // The fake pump delivers one frame per transaction, like the real one
    // runs the rx sink inside the pump.
    if (staged_this_pump > 0) {
        staged_this_pump -= 1;
        const f = [_]u8{ 0xAB, 0xCD, 0xEF };
        _ = link;
        backend.ra8_io_netif_c6link_rx_sink(@as(?*anyopaque, @ptrCast(&ctx)), &f, f.len);
    }
    return 0;
}

extern fn ra8_io_netif_bind_c6link(netif: ?*Bus, backend: ?*Ctx, link: ?*anyopaque, ring: ?[*]u8, ring_bytes: u16) c_int;
extern fn ra8_io_netif_send(netif: ?*const Bus, frame: ?[*]const u8, len: u16) c_int;
extern fn ra8_io_netif_poll(netif: ?*const Bus, out_buf: ?[*]u8, inout_len: ?*u16) c_int;

var fake_link: u8 = 9;

var ctx: Ctx = undefined;
var netif: Bus = undefined;
var ring: [2 * backend.slot_bytes]u8 = undefined;

fn bind() void {
    pump_calls = 0;
    send_calls = 0;
    send_result = 0;
    staged_this_pump = 0;
    ctx = undefined;
    @memset(&ring, 0);
    _ = ra8_io_netif_bind_c6link(&netif, &ctx, @ptrCast(&fake_link), &ring, ring.len);
}

test "bind refuses NULLs and an undersized ring" {
    var n: Bus = .{ .iface = null, .ctx = null };
    var c: Ctx = undefined;
    try std.testing.expectEqual(err_null_ptr, ra8_io_netif_bind_c6link(null, &c, null, &ring, ring.len));
    try std.testing.expectEqual(err_null_ptr, ra8_io_netif_bind_c6link(&n, null, null, &ring, ring.len));
    try std.testing.expectEqual(err_null_ptr, ra8_io_netif_bind_c6link(&n, &c, null, null, ring.len));
    errors_logged = 0;
    try std.testing.expectEqual(err_invalid_arg, ra8_io_netif_bind_c6link(&n, &c, null, &ring, backend.slot_bytes - 1));
    try std.testing.expectEqual(@as(u32, 1), errors_logged);
}

test "bind resets the ring state and dispatches" {
    bind();
    try std.testing.expectEqual(@as(u16, 2), ctx.slots);
    try std.testing.expectEqual(@as(u16, 0), ctx.count);
    try std.testing.expectEqual(@as(?*anyopaque, &ctx), netif.ctx);

    send_calls = 0;
    last_send_link = null;
    last_send_len = 0;
    const frame = [_]u8{ 0x01, 0x02 };
    try std.testing.expectEqual(front.ok, ra8_io_netif_send(&netif, &frame, frame.len));
    try std.testing.expectEqual(@as(u32, 1), send_calls);
    try std.testing.expectEqual(@as(?*anyopaque, @ptrCast(&fake_link)), last_send_link);
    try std.testing.expectEqual(@as(u16, 2), last_send_len);
}

test "poll pumps the link once, then drains staged frames FIFO" {
    bind();
    staged_this_pump = 2; // the fake pump stages a frame per transaction

    var buf: [16]u8 = undefined;
    var len: u16 = @sizeOf(@TypeOf(buf));
    try std.testing.expectEqual(front.ok, ra8_io_netif_poll(&netif, &buf, &len));
    try std.testing.expectEqual(@as(u32, 1), pump_calls);
    try std.testing.expectEqual(@as(u16, 3), len);
    try std.testing.expectEqualSlices(u8, &[_]u8{ 0xAB, 0xCD, 0xEF }, buf[0..len]);

    try std.testing.expectEqual(front.ok, ra8_io_netif_poll(&netif, &buf, &len));
    try std.testing.expectEqual(@as(u32, 2), pump_calls);
    try std.testing.expectEqualSlices(u8, &[_]u8{ 0xAB, 0xCD, 0xEF }, buf[0..len]);
}

test "poll with nothing staged answers no_data" {
    bind();
    var len: u16 = 16;
    try std.testing.expectEqual(err_no_data, ra8_io_netif_poll(&netif, null, &len));
    try std.testing.expectEqual(@as(u32, 1), pump_calls);
}

test "a frame larger than the caller buffer stays staged" {
    bind();
    const f = [_]u8{ 0x11, 0x11, 0x11, 0x11, 0x11 };
    backend.ra8_io_netif_c6link_rx_sink(&ctx, &f, f.len);

    var small: [3]u8 = undefined;
    var len: u16 = @sizeOf(@TypeOf(small));
    try std.testing.expectEqual(err_invalid_size, ra8_io_netif_poll(&netif, &small, &len));
    try std.testing.expectEqual(@as(u16, 1), ctx.count);

    len = 16;
    var big: [16]u8 = undefined;
    try std.testing.expectEqual(front.ok, ra8_io_netif_poll(&netif, &big, &len));
    try std.testing.expectEqual(@as(u16, 5), len);
    try std.testing.expectEqual(@as(u16, 0), ctx.count);
}

test "a full ring counts drops and never blocks the pump" {
    bind();
    const f = [_]u8{ 0x22, 0x33 };
    backend.ra8_io_netif_c6link_rx_sink(&ctx, &f, f.len);
    backend.ra8_io_netif_c6link_rx_sink(&ctx, &f, f.len);
    try std.testing.expectEqual(@as(u16, 2), ctx.count);
    backend.ra8_io_netif_c6link_rx_sink(&ctx, &f, f.len);
    try std.testing.expectEqual(@as(u16, 2), ctx.count);
    try std.testing.expectEqual(@as(u16, 1), ctx.dropped);
    // The pump still runs: the sink refused, not the transport.
    staged_this_pump = 1;
    var buf: [16]u8 = undefined;
    var len: u16 = @sizeOf(@TypeOf(buf));
    try std.testing.expectEqual(front.ok, ra8_io_netif_poll(&netif, &buf, &len));
    try std.testing.expectEqual(@as(u16, 2), ctx.dropped);
}

test "the sink refuses malformed frames" {
    bind();
    const f = [_]u8{ 0x44 };
    backend.ra8_io_netif_c6link_rx_sink(&ctx, null, 4);
    backend.ra8_io_netif_c6link_rx_sink(&ctx, &f, 0);
    backend.ra8_io_netif_c6link_rx_sink(&ctx, &f, 1600);
    try std.testing.expectEqual(@as(u16, 0), ctx.count);
    try std.testing.expectEqual(@as(u16, 0), ctx.dropped);
}

test "a NULL link skips the pump but still drains" {
    var n: Bus = .{ .iface = null, .ctx = null };
    var c: Ctx = undefined;
    _ = ra8_io_netif_bind_c6link(&n, &c, null, &ring, ring.len);
    const f = [_]u8{ 0x55, 0x66 };
    backend.ra8_io_netif_c6link_rx_sink(&c, &f, f.len);
    pump_calls = 0;
    var buf: [16]u8 = undefined;
    var len: u16 = @sizeOf(@TypeOf(buf));
    try std.testing.expectEqual(front.ok, ra8_io_netif_poll(&n, &buf, &len));
    try std.testing.expectEqual(@as(u32, 0), pump_calls);
    try std.testing.expectEqual(@as(u16, 2), len);
}
