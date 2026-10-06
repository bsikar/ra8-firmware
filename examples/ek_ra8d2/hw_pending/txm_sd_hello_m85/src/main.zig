//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! txm_sd_hello_m85 (RA8FW-829, slice 1 of RA8FW-290): CPU0 brings up the
//! micro-SD card on the native SDHI0 bus, mounts its FAT volume through
//! ra8_fs, reads the signed hello-world module `txm_hello_m33.ra8app` into
//! RAM and checks its header. Signature checks and the Module Manager load
//! follow in RA8FW-830. Zig throughout; ra8_fs is the existing C library.

const std = @import("std");

pub const panic = std.debug.no_panic;

pub const baud: u32 = 115_200;
/// The file the build installs as arm/txm_hello_m33.ra8app (RA8FW-479).
pub const file_name = "txm_hello_m33.ra8app";
/// Largest image read into RAM.
pub const file_max = 64 * 1024;
/// sizeof(appimg.Header): eight u32 fields, app_id 32, display_name 32,
/// signature 64 (libs/ra8_app/src/internal/appimg.zig).
pub const header_bytes = 160;
/// appimg.Header.magic, "A8AR" read little-endian.
pub const appimg_magic: u32 = 0x5241_3841;
const code_size_offset = 12;
const data_size_offset = 16;

const sdhi_instance: u8 = 0;
const bus_width_4bit: u8 = 4;
const block_bytes: u32 = 512;
const mode_read: c_uint = 0;
const ok: u16 = 0;
const err_invalid_arg: u16 = 0x103;
const err_out_of_range: u16 = 0x208;

/// `ra8_fs_backend_t` (libs/ra8_fs/inc/ra8_fs_types.h), field order exact.
const FsBackend = extern struct {
    read_block: *const fn (?*anyopaque, u64, u32, ?[*]u8) callconv(.c) u16,
    write_block: *const fn (?*anyopaque, u64, u32, ?[*]const u8) callconv(.c) u16,
    get_capacity: *const fn (?*anyopaque, ?*u64, ?*u32) callconv(.c) u16,
    erase_blocks: ?*const fn (?*anyopaque, u64, u64) callconv(.c) u16 = null,
    ctx: ?*anyopaque = null,
};

/// `ra8_sdcard_cfg_t`.
const SdCfg = extern struct { instance: u8, bus_width: u8 };

const Fail = error{ pins, card, mount, open, size, read, header };

extern fn ra8_cgc_init() c_int;
extern fn ra8_board_uart_console_init(baud: u32) c_int;
extern fn ra8_board_uart_console_write(data: [*]const u8, len: usize) c_int;
extern fn ra8_board_uart_console_flush() c_int;
extern fn ra8_board_sdhi_pins_init() u16;
extern fn ra8_sdcard_init(cfg: *const SdCfg) u16;
extern fn ra8_sdcard_read_blocks(lba: u32, buf: ?[*]u8, count: u32) u16;
extern fn ra8_sdcard_write_blocks(lba: u32, buf: ?[*]const u8, count: u32) u16;
extern fn ra8_sdcard_get_capacity(out_blocks: *u32) u16;
extern fn ra8_fs_mount(backend: *const FsBackend, out_handle: *?*anyopaque) u16;
extern fn ra8_fs_open(handle: ?*anyopaque, path: [*:0]const u8, mode: c_uint, out_file: *?*anyopaque) u16;
extern fn ra8_fs_size(file: ?*anyopaque, out_bytes: *u64) u16;
extern fn ra8_fs_read(file: ?*anyopaque, buf: [*]u8, max_len: u32, got_len: *u32) u16;
extern fn ra8_fs_close(file: ?*anyopaque) u16;

var image: [file_max]u8 align(4) = undefined;

fn readBlocks(ctx: ?*anyopaque, lba: u64, count: u32, buf: ?[*]u8) callconv(.c) u16 {
    _ = ctx;
    const first = std.math.cast(u32, lba) orelse return err_out_of_range;
    return ra8_sdcard_read_blocks(first, buf, count);
}

fn writeBlocks(ctx: ?*anyopaque, lba: u64, count: u32, buf: ?[*]const u8) callconv(.c) u16 {
    _ = ctx;
    const first = std.math.cast(u32, lba) orelse return err_out_of_range;
    return ra8_sdcard_write_blocks(first, buf, count);
}

fn capacity(ctx: ?*anyopaque, out_blocks: ?*u64, out_size: ?*u32) callconv(.c) u16 {
    _ = ctx;
    const blocks_out = out_blocks orelse return err_invalid_arg;
    const size_out = out_size orelse return err_invalid_arg;
    var blocks: u32 = 0;
    const err = ra8_sdcard_get_capacity(&blocks);
    if (err != ok) return err;
    blocks_out.* = blocks;
    size_out.* = block_bytes;
    return ok;
}

const backend = FsBackend{ .read_block = &readBlocks, .write_block = &writeBlocks, .get_capacity = &capacity };

/// True when `bytes` opens with an .ra8app header whose declared code and
/// data sizes account for exactly the rest of the file.
pub fn headerOk(bytes: []const u8) bool {
    if (bytes.len < header_bytes) return false;
    if (std.mem.readInt(u32, bytes[0..4], .little) != appimg_magic) return false;
    const code = std.mem.readInt(u32, bytes[code_size_offset..][0..4], .little);
    const data = std.mem.readInt(u32, bytes[data_size_offset..][0..4], .little);
    return @as(u64, header_bytes) + code + data == bytes.len;
}

fn openCard() Fail!?*anyopaque {
    if (ra8_board_sdhi_pins_init() != ok) return error.pins;
    const cfg = SdCfg{ .instance = sdhi_instance, .bus_width = bus_width_4bit };
    if (ra8_sdcard_init(&cfg) != ok) return error.card;
    var mount: ?*anyopaque = null;
    if (ra8_fs_mount(&backend, &mount) != ok) return error.mount;
    return mount;
}

fn readApp(buf: []u8) Fail!usize {
    const mount = try openCard();
    var file: ?*anyopaque = null;
    if (ra8_fs_open(mount, file_name, mode_read, &file) != ok) return error.open;
    defer _ = ra8_fs_close(file);
    var size: u64 = 0;
    if (ra8_fs_size(file, &size) != ok or size > buf.len) return error.size;
    var got: u32 = 0;
    if (ra8_fs_read(file, buf.ptr, @intCast(size), &got) != ok or got != size) return error.read;
    if (!headerOk(buf[0..got])) return error.header;
    return got;
}

fn say(line: []const u8) void {
    _ = ra8_board_uart_console_write(line.ptr, line.len);
    _ = ra8_board_uart_console_flush();
}

fn report(result: Fail!usize) void {
    var line: [64]u8 = undefined;
    const text = if (result) |bytes|
        std.fmt.bufPrint(&line, "txm_sd_hello_m85: read {d} bytes, header ok PASS\r\n", .{bytes})
    else |err|
        std.fmt.bufPrint(&line, "txm_sd_hello_m85: FAIL {s}\r\n", .{@errorName(err)});
    say(text catch "txm_sd_hello_m85: FAIL\r\n");
}

fn park() noreturn {
    while (true) asm volatile ("wfi");
}

export fn main() callconv(.c) c_int {
    if (ra8_cgc_init() != 0) park();
    _ = ra8_board_uart_console_init(baud);
    report(readApp(&image));
    park();
}
