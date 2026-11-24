//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for `epub_img_import_abi.zig`.

const std = @import("std");
const testing = std.testing;
const sut = @import("epub_img_import_abi");

const epub_atlas_import_cfg_t = sut.epub_atlas_import_cfg_t;
const epub_book_t = sut.epub_book_t;
const epub_entry_pull_t = sut.epub_entry_pull_t;
const epub_tile_binder_import = sut.epub_tile_binder_import;
const epub_tile_binder_t = sut.epub_tile_binder_t;
const internal_entry_pull = sut.internal_entry_pull;
const k_epub_import_magic_len = sut.k_epub_import_magic_len;
const k_ra8_err_invalid_arg = sut.k_ra8_err_invalid_arg;
const k_ra8_err_not_found = sut.k_ra8_err_not_found;
const k_ra8_err_not_supported = sut.k_ra8_err_not_supported;
const k_ra8_err_null_ptr = sut.k_ra8_err_null_ptr;
const k_ra8_ok = sut.k_ra8_ok;
const ra8_err_t = sut.ra8_err_t;

var t_work_buf: [4096]u8 = undefined;
const t_sink_ctx_addr: *anyopaque = @ptrFromInt(0x1000);
const t_pread_ctx_addr: *anyopaque = @ptrFromInt(0x2000);

fn t_dummy_sink(_ctx: ?*anyopaque, _bytes: ?[*]const u8, _len: usize) callconv(.c) ra8_err_t {
    _ = _ctx;
    _ = _bytes;
    _ = _len;
    return k_ra8_ok;
}

fn t_dummy_pread(_ctx: ?*anyopaque, _offset: u64, _buf: ?[*]u8, _len: usize, _got: ?*usize) callconv(.c) ra8_err_t {
    _ = _ctx;
    _ = _offset;
    _ = _buf;
    _ = _len;
    _ = _got;
    return k_ra8_err_not_supported;
}

fn t_cfg() epub_atlas_import_cfg_t {
    return .{
        .tile_w = 128,
        .tile_h = 96,
        .codec = 1,
        .max_width = 2048,
        .max_height = 1536,
        .work = &t_work_buf,
        .work_cap = t_work_buf.len,
        .webp_work = null,
        .webp_work_cap = 0,
        .store = .{
            .sink = t_dummy_sink,
            .sink_ctx = t_sink_ctx_addr,
            .pread = t_dummy_pread,
            .pread_ctx = t_pread_ctx_addr,
        },
    };
}

fn t_cfg_no_sink() epub_atlas_import_cfg_t {
    var cfg = t_cfg();
    cfg.store.sink = null;
    return cfg;
}

fn t_cfg_no_pread() epub_atlas_import_cfg_t {
    var cfg = t_cfg();
    cfg.store.pread = null;
    return cfg;
}

fn t_binder() ?*epub_tile_binder_t {
    return @ptrFromInt(0x3000);
}

fn t_book() ?*epub_book_t {
    return @ptrFromInt(0x4000);
}

extern fn teststub_reset() void;

extern var teststub_open_calls: c_int;

extern var teststub_read_calls: c_int;

extern var teststub_close_calls: c_int;

extern var teststub_pread_calls: c_int;

extern var teststub_add_calls: c_int;

extern var teststub_add_ext_calls: c_int;

extern var teststub_produce_calls: c_int;

extern var teststub_open_result: u16;

extern var teststub_close_result: u16;

extern var teststub_pread_result: u16;

extern var teststub_add_result: u16;

extern var teststub_add_ext_result: u16;

extern var teststub_produce_result: u16;

extern var teststub_entry_size: u64;

extern var teststub_read_remaining: usize;

extern var teststub_read_fill: u8;

extern var teststub_read_cap_seen: usize;

extern var teststub_produced_total: u32;

extern var teststub_add_image_id: u32;

extern var teststub_add_ext_total: u64;

extern var teststub_add_ext_image_id: u32;

extern var teststub_add_ext_pread_seen: c_int;

extern var teststub_add_ext_ctx_seen: c_int;

extern var teststub_pcfg_tile_w: u16;

extern var teststub_pcfg_tile_h: u16;

extern var teststub_pcfg_codec: u8;

extern var teststub_pcfg_max_w: u16;

extern var teststub_pcfg_max_h: u16;

extern var teststub_pcfg_work_cap: usize;

extern var teststub_pcfg_webp_cap: usize;

extern var teststub_pcfg_work_seen: c_int;

extern var teststub_pcfg_webp_seen: c_int;

extern var teststub_pcfg_sink_seen: c_int;

extern var teststub_pcfg_sink_ctx_seen: c_int;

extern var teststub_pcfg_pull_ctx_seen: c_int;

extern var teststub_pread_buf: [k_epub_import_magic_len]u8;

extern var teststub_pread_len: usize;

test "null arguments are rejected before any extern call" {
    teststub_reset();
    const cfg = t_cfg();
    var no_sink = t_cfg_no_sink();
    var no_pread = t_cfg_no_pread();
    try std.testing.expectEqual(k_ra8_err_null_ptr, epub_tile_binder_import(null, t_book(), "x.jpg", 7, &cfg));
    try std.testing.expectEqual(k_ra8_err_null_ptr, epub_tile_binder_import(t_binder(), null, "x.jpg", 7, &cfg));
    try std.testing.expectEqual(k_ra8_err_null_ptr, epub_tile_binder_import(t_binder(), t_book(), null, 7, &cfg));
    try std.testing.expectEqual(k_ra8_err_null_ptr, epub_tile_binder_import(t_binder(), t_book(), "x.jpg", 7, null));
    try std.testing.expectEqual(k_ra8_err_null_ptr, epub_tile_binder_import(t_binder(), t_book(), "x.jpg", 7, &no_sink));
    try std.testing.expectEqual(k_ra8_err_null_ptr, epub_tile_binder_import(t_binder(), t_book(), "x.jpg", 7, &no_pread));
    try std.testing.expectEqual(@as(c_int, 0), teststub_pread_calls + teststub_open_calls + teststub_add_calls + teststub_produce_calls);
}

test "empty and over-long hrefs are invalid" {
    teststub_reset();
    const cfg = t_cfg();
    try std.testing.expectEqual(k_ra8_err_invalid_arg, epub_tile_binder_import(t_binder(), t_book(), "", 7, &cfg));
    var long_href: [193]u8 = @splat('a');
    long_href[192] = 0;
    const long: [*:0]const u8 = @ptrCast(&long_href);
    try std.testing.expectEqual(k_ra8_err_invalid_arg, epub_tile_binder_import(t_binder(), t_book(), long, 7, &cfg));
    // A 191-char href is exactly at the limit: it reaches the classify step
    // and propagates the (stubbed) not-found result.
    teststub_pread_result = k_ra8_err_not_found;
    var edge_href: [192]u8 = @splat('b');
    edge_href[191] = 0;
    const edge: [*:0]const u8 = @ptrCast(&edge_href);
    try std.testing.expectEqual(k_ra8_err_not_found, epub_tile_binder_import(t_binder(), t_book(), edge, 7, &cfg));
}

test "missing entry propagates not found" {
    teststub_reset();
    teststub_pread_result = k_ra8_err_not_found;
    const cfg = t_cfg();
    try std.testing.expectEqual(k_ra8_err_not_found, epub_tile_binder_import(t_binder(), t_book(), "gone.jpg", 7, &cfg));
    try std.testing.expectEqual(@as(c_int, 1), teststub_pread_calls);
    try std.testing.expectEqual(@as(c_int, 0), teststub_open_calls + teststub_produce_calls + teststub_add_calls);
}

test "stored atlas registers in place" {
    teststub_reset();
    @memcpy(&teststub_pread_buf, "JOF1");
    teststub_pread_len = k_epub_import_magic_len;
    const cfg = t_cfg();
    try std.testing.expectEqual(k_ra8_ok, epub_tile_binder_import(t_binder(), t_book(), "page.jof", 9, &cfg));
    try std.testing.expectEqual(@as(c_int, 1), teststub_add_calls);
    try std.testing.expectEqual(@as(u32, 9), teststub_add_image_id);
    try std.testing.expectEqual(@as(c_int, 0), teststub_open_calls + teststub_produce_calls + teststub_add_ext_calls);
}

test "deflated entry falls through to the transcode" {
    teststub_reset();
    teststub_pread_result = k_ra8_err_not_supported;
    teststub_produced_total = 5321;
    const cfg = t_cfg();
    try std.testing.expectEqual(k_ra8_ok, epub_tile_binder_import(t_binder(), t_book(), "scan.jpg", 11, &cfg));
    try std.testing.expectEqual(@as(c_int, 1), teststub_open_calls);
    try std.testing.expectEqual(@as(c_int, 1), teststub_produce_calls);
    try std.testing.expectEqual(@as(c_int, 1), teststub_close_calls);
    try std.testing.expectEqual(@as(c_int, 1), teststub_add_ext_calls);
    try std.testing.expectEqual(@as(u64, 5321), teststub_add_ext_total);
    try std.testing.expectEqual(@as(u32, 11), teststub_add_ext_image_id);
    try std.testing.expectEqual(@as(c_int, 1), teststub_add_ext_pread_seen);
    try std.testing.expectEqual(@as(c_int, 1), teststub_add_ext_ctx_seen);
    try std.testing.expectEqual(@as(c_int, 0), teststub_add_calls);
    // The producer config mirrors the import knobs.
    try std.testing.expectEqual(@as(u16, 128), teststub_pcfg_tile_w);
    try std.testing.expectEqual(@as(u16, 96), teststub_pcfg_tile_h);
    try std.testing.expectEqual(@as(u8, 1), teststub_pcfg_codec);
    try std.testing.expectEqual(@as(u16, 2048), teststub_pcfg_max_w);
    try std.testing.expectEqual(@as(u16, 1536), teststub_pcfg_max_h);
    try std.testing.expectEqual(@as(usize, 4096), teststub_pcfg_work_cap);
    try std.testing.expectEqual(@as(c_int, 1), teststub_pcfg_sink_seen);
    try std.testing.expectEqual(@as(c_int, 1), teststub_pcfg_sink_ctx_seen);
    try std.testing.expectEqual(@as(c_int, 1), teststub_pcfg_pull_ctx_seen);
    try std.testing.expectEqual(@as(c_int, 0), teststub_pcfg_webp_seen);
}

test "producer error wins over the close error" {
    teststub_reset();
    teststub_pread_result = k_ra8_err_not_supported;
    teststub_produce_result = k_ra8_err_not_found;
    const cfg = t_cfg();
    try std.testing.expectEqual(k_ra8_err_not_found, epub_tile_binder_import(t_binder(), t_book(), "scan.jpg", 3, &cfg));
    try std.testing.expectEqual(@as(c_int, 1), teststub_close_calls);
    try std.testing.expectEqual(@as(c_int, 0), teststub_add_ext_calls);
}

test "close error surfaces when the producer succeeded" {
    teststub_reset();
    teststub_pread_result = k_ra8_err_not_supported;
    teststub_close_result = k_ra8_err_not_found;
    const cfg = t_cfg();
    try std.testing.expectEqual(k_ra8_err_not_found, epub_tile_binder_import(t_binder(), t_book(), "scan.jpg", 3, &cfg));
    try std.testing.expectEqual(@as(c_int, 1), teststub_close_calls);
    try std.testing.expectEqual(@as(c_int, 0), teststub_add_ext_calls);
}

test "the pull adapter delegates to the entry cursor" {
    teststub_reset();
    teststub_entry_size = 10000;
    teststub_read_remaining = 10;
    teststub_read_fill = 0x5A;
    var pull = std.mem.zeroes(epub_entry_pull_t);
    var buf: [16]u8 = @splat(0);
    var got: usize = 0;
    try std.testing.expectEqual(k_ra8_ok, internal_entry_pull(@ptrCast(&pull), &buf, buf.len, &got));
    try std.testing.expectEqual(@as(usize, 10), got);
    try std.testing.expectEqual(@as(c_int, 1), teststub_read_calls);
    try std.testing.expectEqual(@as(usize, 16), teststub_read_cap_seen);
    for (buf[0..10]) |b| try std.testing.expectEqual(@as(u8, 0x5A), b);
    // End of entry: a clean 0-byte read.
    try std.testing.expectEqual(k_ra8_ok, internal_entry_pull(@ptrCast(&pull), &buf, buf.len, &got));
    try std.testing.expectEqual(@as(usize, 0), got);
    try std.testing.expectEqual(@as(c_int, 2), teststub_read_calls);
}
