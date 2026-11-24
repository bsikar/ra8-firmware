//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Native-SDHI ra8_io mount-or-format, root listing, and VFS round-trip for
//! `ra8_io_sdhi_demo`.

const std = @import("std");

const ok: u16 = 0;
const err_fail: u16 = 0x101;
const err_invalid_size: u16 = 0x105;
const err_exists: u16 = 0x10C;
const err_checksum_mismatch: u16 = 0x502;

const fs_type_fat16: u8 = 16;
const fs_attr_volume_id: u8 = 0x08;
const fs_mode_read: u8 = 0;
const fs_mode_write: u8 = 1;

const payload_bytes: u32 = 512;
const prng_seed: u32 = 0xA5F00DAD;
const prng_mul: u32 = 1664525;
const prng_add: u32 = 1013904223;
const prng_byte_shift: u5 = 16;

const dir_path: [*:0]const u8 = "sd:/LOGS";
const file_path: [*:0]const u8 = "sd:/LOGS/A.TXT";

const Blockdev = extern struct {
    iface: ?*const anyopaque = null,
    ctx: ?*anyopaque = null,
};

const FsBackend = extern struct {
    read_block: ?*const anyopaque = null,
    write_block: ?*const anyopaque = null,
    get_capacity: ?*const anyopaque = null,
    erase_blocks: ?*const anyopaque = null,
    ctx: ?*anyopaque = null,
};

const FormatOpts = extern struct {
    type: u8,
    label: ?[*:0]const u8 = null,
    sectors_per_cluster: u8 = 0,
};

const ListdirCb = *const fn (
    name: ?[*:0]const u8,
    attr: u8,
    size: u64,
    ctx: ?*anyopaque,
) callconv(.c) void;

extern fn ra8_board_uart_console_write(msg: ?[*]const u8, len: usize) callconv(.c) u32;
extern fn ra8_io_vfs_init() callconv(.c) c_int;
extern fn ra8_io_blockdev_sdhi_init(bd: ?*Blockdev) callconv(.c) c_int;
extern fn ra8_io_blockdev_as_fs_backend(bd: ?*const Blockdev, out: ?*FsBackend) callconv(.c) c_int;
extern fn ra8_io_vfs_mount(name: ?[*:0]const u8, mount: ?*anyopaque) callconv(.c) c_int;
extern fn ra8_io_vfs_listdir(
    path: ?[*:0]const u8,
    cb: ?ListdirCb,
    ctx: ?*anyopaque,
) callconv(.c) c_int;
extern fn ra8_io_vfs_mkdir(path: ?[*:0]const u8) callconv(.c) c_int;
extern fn ra8_io_vfs_open(
    path: ?[*:0]const u8,
    mode: u8,
    out_file: ?*?*anyopaque,
) callconv(.c) c_int;
extern fn ra8_fs_format(backend: ?*const FsBackend, opts: ?*const FormatOpts) callconv(.c) u16;
extern fn ra8_fs_mount(backend: ?*const FsBackend, out_handle: ?*?*anyopaque) callconv(.c) u16;
extern fn ra8_fs_write(file: ?*anyopaque, buf: ?[*]const u8, len: u32) callconv(.c) u16;
extern fn ra8_fs_read(
    file: ?*anyopaque,
    buf: ?[*]u8,
    max_len: u32,
    got_len: ?*u32,
) callconv(.c) u16;
extern fn ra8_fs_close(file: ?*anyopaque) callconv(.c) u16;

var s_payload: [payload_bytes]u8 = undefined;
var s_readback: [payload_bytes]u8 = undefined;
var s_bd: Blockdev = .{};
var s_be: FsBackend = .{};

fn ioErr(rc: c_int) u16 {
    if (rc < 0 or rc > 0xFFFF) return err_fail;
    return @intCast(rc);
}

fn writeConsole(slice: []const u8) void {
    if (slice.len == 0) return;
    _ = ra8_board_uart_console_write(slice.ptr, slice.len);
}

fn formatU64(buf: *[20]u8, value: u64) []const u8 {
    if (value == 0) {
        buf[0] = '0';
        return buf[0..1];
    }
    var v = value;
    var i: usize = buf.len;
    while (v > 0) {
        i -= 1;
        buf[i] = @as(u8, @intCast(v % 10)) + '0';
        v /= 10;
    }
    return buf[i..buf.len];
}

fn fillPayload() void {
    var state: u32 = prng_seed;
    for (&s_payload) |*slot| {
        state = (state *% prng_mul) +% prng_add;
        slot.* = @truncate(state >> prng_byte_shift);
    }
}

fn listdirEntryCb(
    name: ?[*:0]const u8,
    attr: u8,
    size: u64,
    ctx: ?*anyopaque,
) callconv(.c) void {
    _ = ctx;
    // Skip FAT volume-label metadata entries (ATTR_VOLUME_ID = 0x08).
    if ((attr & fs_attr_volume_id) != 0) return;
    const entry_name = name orelse return;
    const name_slice = std.mem.span(entry_name);
    var dec_buf: [20]u8 = undefined;
    const size_slice = formatU64(&dec_buf, size);

    const prefix = "ra8_io_sdhi_demo: entry ";
    const suffix = " bytes\r\n";
    var line_buf: [800]u8 = undefined;
    if (prefix.len + name_slice.len + 1 + size_slice.len + suffix.len > line_buf.len) {
        writeConsole(prefix);
        writeConsole(name_slice);
        writeConsole(" ");
        writeConsole(size_slice);
        writeConsole(suffix);
        return;
    }

    var pos: usize = 0;
    @memcpy(line_buf[pos .. pos + prefix.len], prefix);
    pos += prefix.len;
    @memcpy(line_buf[pos .. pos + name_slice.len], name_slice);
    pos += name_slice.len;
    line_buf[pos] = ' ';
    pos += 1;
    @memcpy(line_buf[pos .. pos + size_slice.len], size_slice);
    pos += size_slice.len;
    @memcpy(line_buf[pos .. pos + suffix.len], suffix);
    pos += suffix.len;
    writeConsole(line_buf[0..pos]);
}

/// Bind SDHI blockdev, mount any existing FAT volume (formatting FAT16 only
/// when unformatted/blank), and publish it under `"sd"`.
fn mountOrFormatViaIo() u16 {
    var rc = ra8_io_vfs_init();
    if (rc != 0) return ioErr(rc);

    rc = ra8_io_blockdev_sdhi_init(&s_bd);
    if (rc != 0) return ioErr(rc);

    rc = ra8_io_blockdev_as_fs_backend(&s_bd, &s_be);
    if (rc != 0) return ioErr(rc);

    var mnt: ?*anyopaque = null;
    var fs_err = ra8_fs_mount(&s_be, &mnt);
    if (fs_err != ok or mnt == null) {
        const opts = FormatOpts{
            .type = fs_type_fat16,
            .label = "RAIOSD",
            .sectors_per_cluster = 0,
        };
        fs_err = ra8_fs_format(&s_be, &opts);
        if (fs_err != ok) return fs_err;

        mnt = null;
        fs_err = ra8_fs_mount(&s_be, &mnt);
        if (fs_err != ok) return fs_err;
    }

    rc = ra8_io_vfs_mount("sd", mnt);
    return if (rc == 0) ok else ioErr(rc);
}

fn listRootDir() u16 {
    const rc = ra8_io_vfs_listdir("sd:/", listdirEntryCb, null);
    return if (rc == 0) ok else ioErr(rc);
}

fn ensureDir(path: [*:0]const u8) u16 {
    const rc = ra8_io_vfs_mkdir(path);
    if (rc == 0 or rc == err_exists) return ok;
    return ioErr(rc);
}

fn writePayload(path: [*:0]const u8) u16 {
    var wf: ?*anyopaque = null;
    const open_rc = ra8_io_vfs_open(path, fs_mode_write, &wf);
    if (open_rc != 0) return ioErr(open_rc);

    const w_err = ra8_fs_write(wf, &s_payload, payload_bytes);
    const c_err = ra8_fs_close(wf);
    return if (w_err != ok) w_err else c_err;
}

fn readAndVerify(path: [*:0]const u8) u16 {
    var rf: ?*anyopaque = null;
    const open_rc = ra8_io_vfs_open(path, fs_mode_read, &rf);
    if (open_rc != 0) return ioErr(open_rc);

    @memset(&s_readback, 0);
    var got: u32 = 0;
    const r_err = ra8_fs_read(rf, &s_readback, payload_bytes, &got);
    const c_err = ra8_fs_close(rf);
    if (r_err != ok) return r_err;
    if (c_err != ok) return c_err;
    if (got != payload_bytes) return err_invalid_size;
    if (!std.mem.eql(u8, &s_payload, &s_readback)) return err_checksum_mismatch;
    return ok;
}

/// Mount (or format if blank), list `sd:/`, and run the `sd:/LOGS/A.TXT` round-trip.
pub export fn sdhi_demo_run() callconv(.c) u16 {
    fillPayload();

    var err = mountOrFormatViaIo();
    if (err != ok) return err;

    err = listRootDir();
    if (err != ok) return err;

    err = ensureDir(dir_path);
    if (err != ok) return err;

    err = writePayload(file_path);
    if (err != ok) return err;

    return readAndVerify(file_path);
}
