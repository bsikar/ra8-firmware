//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Import-time transcode wiring (RA8FW-1193), ported from
//! src/epub_img_import.c behind the unchanged inc/epub_img_tiles.h and
//! inc/epub_entry.h contracts. Semantics and error paths match the C TU
//! exactly: resolve a manifest image href, register it in place when the
//! entry already is a stored JOF atlas, otherwise stream the encoded source
//! through jof_produce into the caller's atlas store and register the result
//! against the store's own read seam. The C TU's
//! __has_include("ra8_tile_cache.h") && __has_include("jof.h") emptiness
//! contract moves to the build: this TU ships as its own archive
//! (ra8_epub_img_import) whose members are pulled only when a consumer calls
//! epub_tile_binder_import, so an epub-only app that never links jof keeps
//! the same link surface the empty C translation unit gave it. The C TU's
//! per-argument log lines are dropped here, as in the other abi slices; the
//! returned codes are identical.

const std = @import("std");

// ---- ra8_err_t (libs/ra8_core/inc/ra8_err.h, C23 enum : uint16_t) -----------
const ra8_err_t = u16;
const k_ra8_ok: ra8_err_t = 0;
const k_ra8_err_invalid_arg: ra8_err_t = 0x103;
const k_ra8_err_not_found: ra8_err_t = 0x106;
const k_ra8_err_not_supported: ra8_err_t = 0x107;
const k_ra8_err_null_ptr: ra8_err_t = 0x504;

// ---- epub.h / epub_img_tiles.h / epub_entry.h (exact C layout) --------------
const k_epub_max_path_len = 192;

/// Import-path sniffing constants (epub_import_const_t).
const k_epub_import_magic_len = 4;

const epub_book_t = opaque {};
const epub_tile_binder_t = opaque {};

const jof_sink_fn = *const fn (?*anyopaque, ?[*]const u8, usize) callconv(.c) ra8_err_t;
const jof_pread_fn = *const fn (?*anyopaque, u64, ?[*]u8, usize, ?*usize) callconv(.c) ra8_err_t;
const jof_pull_fn = *const fn (?*anyopaque, ?[*]u8, usize, ?*usize) callconv(.c) ra8_err_t;

/// epub_atlas_store_t (epub_img_tiles.h): sink + pread over one backing store.
const epub_atlas_store_t = extern struct {
    sink: ?jof_sink_fn,
    sink_ctx: ?*anyopaque,
    pread: ?jof_pread_fn,
    pread_ctx: ?*anyopaque,
};

/// epub_atlas_import_cfg_t (epub_img_tiles.h): the producer knobs minus the
/// source, which the import path streams from the archive entry itself.
const epub_atlas_import_cfg_t = extern struct {
    tile_w: u16,
    tile_h: u16,
    codec: u8,
    max_width: u16,
    max_height: u16,
    work: ?[*]u8,
    work_cap: usize,
    webp_work: ?[*]u8,
    webp_work_cap: usize,
    store: epub_atlas_store_t,
};

/// epub_entry_reader_t (epub_entry.h): forward streaming cursor over one
/// ZIP entry, 8-byte aligned with the reserved tail rounding `done` up.
const epub_entry_reader_t = extern struct {
    iter: ?*anyopaque,
    book: ?*epub_book_t,
    total: u64,
    consumed: u64,
    done: u8,
    reserved: [7]u8,
};

/// jof_info_t (jof.h): produced atlas geometry.
const jof_info_t = extern struct {
    width: u16,
    height: u16,
    tile_w: u16,
    tile_h: u16,
    tile_cols: u16,
    tile_rows: u16,
    bpp: u8,
    codec: u8,
    tile_count: u32,
    index_off: u32,
    total_size: u32,
};

/// jof_produce_cfg_t (jof_produce.h): the producer configuration.
const jof_produce_cfg_t = extern struct {
    pull: ?jof_pull_fn,
    pull_ctx: ?*anyopaque,
    sink: ?jof_sink_fn,
    sink_ctx: ?*anyopaque,
    tile_w: u16,
    tile_h: u16,
    codec: u8,
    max_width: u16,
    max_height: u16,
    work: ?[*]u8,
    work_cap: usize,
    webp_work: ?[*]u8,
    webp_work_cap: usize,
};

// ---- extern surface (resolved at the final link) ----------------------------
extern fn epub_entry_open(book: ?*epub_book_t, path: ?[*:0]const u8, out_reader: ?*epub_entry_reader_t, out_size: ?*u64) callconv(.c) ra8_err_t;
extern fn epub_entry_read(reader: ?*epub_entry_reader_t, buf: ?[*]u8, cap: usize, got: ?*usize) callconv(.c) ra8_err_t;
extern fn epub_entry_close(reader: ?*epub_entry_reader_t) callconv(.c) ra8_err_t;
extern fn epub_entry_pread(book: ?*epub_book_t, path: ?[*:0]const u8, offset: u64, buf: ?[*]u8, len: usize, got: ?*usize) callconv(.c) ra8_err_t;
extern fn epub_tile_binder_add(binder: ?*epub_tile_binder_t, book: ?*epub_book_t, path: ?[*:0]const u8, image_id: u32) callconv(.c) ra8_err_t;
extern fn epub_tile_binder_add_ext(binder: ?*epub_tile_binder_t, pread: ?jof_pread_fn, pread_ctx: ?*anyopaque, total_size: u64, image_id: u32) callconv(.c) ra8_err_t;
extern fn jof_produce(cfg: ?*const jof_produce_cfg_t, out_info: ?*jof_info_t) callconv(.c) ra8_err_t;

comptime {
    // Pointer-shaped like the C header: 40 B on the probed 64-bit host,
    // 32 B under AAPCS (32-bit pointers, u64 still 8-aligned).
    const k_reader_size: usize = if (@sizeOf(usize) == 8) 40 else 32;
    if (@sizeOf(epub_entry_reader_t) != k_reader_size)
        @compileError("epub_entry_reader_t layout drift");
    if (@sizeOf(jof_info_t) != 28)
        @compileError("jof_info_t layout drift");
    const ptr_bytes = @sizeOf(usize);
    const store_bytes = 4 * ptr_bytes;
    const scalar_tail: usize = if (ptr_bytes == 8) 48 else 28;
    if (@sizeOf(epub_atlas_store_t) != store_bytes)
        @compileError("epub_atlas_store_t layout drift");
    if (@sizeOf(epub_atlas_import_cfg_t) != scalar_tail + store_bytes)
        @compileError("epub_atlas_import_cfg_t layout drift");
    if (@sizeOf(jof_produce_cfg_t) != 4 * ptr_bytes + scalar_tail)
        @compileError("jof_produce_cfg_t layout drift");
}

// ---- private helpers --------------------------------------------------------

/// jof_pull_fn adapter over a streaming entry cursor: delegates each chunk to
/// the open cursor; end-of-entry surfaces as a 0-byte read.
const epub_entry_pull_t = extern struct {
    reader: epub_entry_reader_t,
};

fn internal_entry_pull(ctx: ?*anyopaque, buf: ?[*]u8, cap: usize, got: ?*usize) callconv(.c) ra8_err_t {
    const pull: *epub_entry_pull_t = @ptrCast(@alignCast(ctx.?));
    return epub_entry_read(&pull.reader, buf, cap, got);
}

/// Classify an entry: stored JOF atlas (register in place) or not. A 4-byte
/// positioned read succeeds only for stored entries; a DEFLATE-compressed
/// entry reports k_ra8_err_not_supported, which classifies as "not an
/// in-place atlas" (the transcode path).
fn internal_classify(book: ?*epub_book_t, href: [*:0]const u8, out_is_atlas: *bool) ra8_err_t {
    out_is_atlas.* = false;
    var magic: [k_epub_import_magic_len]u8 = @splat(0);
    var got: usize = 0;
    const err = epub_entry_pread(book, href, 0, &magic, magic.len, &got);
    if (err == k_ra8_err_not_supported) {
        return k_ra8_ok; // deflated entry: cannot be an in-place atlas
    }
    if (err != k_ra8_ok) {
        return err;
    }
    if (got == magic.len and std.mem.eql(u8, &magic, "JOF1")) {
        out_is_atlas.* = true;
    }
    return k_ra8_ok;
}

/// Run the producer over an open entry cursor into the atlas store: builds
/// the producer configuration from the import knobs, streams the entry, and
/// always closes the cursor (the first error wins).
fn internal_transcode(book: ?*epub_book_t, href: [*:0]const u8, cfg: *const epub_atlas_import_cfg_t, out_info: *jof_info_t) ra8_err_t {
    var pull = std.mem.zeroes(epub_entry_pull_t);
    var size: u64 = 0;
    var err = epub_entry_open(book, href, &pull.reader, &size);
    if (err != k_ra8_ok) {
        return err;
    }
    const pcfg = jof_produce_cfg_t{
        .pull = internal_entry_pull,
        .pull_ctx = &pull,
        .sink = cfg.store.sink,
        .sink_ctx = cfg.store.sink_ctx,
        .tile_w = cfg.tile_w,
        .tile_h = cfg.tile_h,
        .codec = cfg.codec,
        .max_width = cfg.max_width,
        .max_height = cfg.max_height,
        .work = cfg.work,
        .work_cap = cfg.work_cap,
        .webp_work = cfg.webp_work,
        .webp_work_cap = cfg.webp_work_cap,
    };
    err = jof_produce(&pcfg, out_info);
    const close_err = epub_entry_close(&pull.reader);
    return if (err != k_ra8_ok) err else close_err;
}

/// Reject any NULL import argument (pointer guards, split out so the public
/// entry stays under the C TU's statement budget shape).
fn internal_import_args_ok(binder: ?*epub_tile_binder_t, book: ?*epub_book_t, href: ?[*:0]const u8, cfg: ?*const epub_atlas_import_cfg_t) ra8_err_t {
    if (binder == null) {
        return k_ra8_err_null_ptr;
    }
    if (book == null) {
        return k_ra8_err_null_ptr;
    }
    if (href == null) {
        return k_ra8_err_null_ptr;
    }
    if (cfg == null) {
        return k_ra8_err_null_ptr;
    }
    if (cfg.?.store.sink == null) {
        return k_ra8_err_null_ptr;
    }
    if (cfg.?.store.pread == null) {
        return k_ra8_err_null_ptr;
    }
    return k_ra8_ok;
}

pub export fn epub_tile_binder_import(binder: ?*epub_tile_binder_t, book: ?*epub_book_t, href: ?[*:0]const u8, image_id: u32, cfg: ?*const epub_atlas_import_cfg_t) callconv(.c) ra8_err_t {
    const nz = internal_import_args_ok(binder, book, href, cfg);
    if (nz != k_ra8_ok) {
        return nz;
    }
    const h = href.?;
    const hlen = std.mem.len(h);
    if (hlen == 0 or hlen >= k_epub_max_path_len) {
        return k_ra8_err_invalid_arg;
    }
    var is_atlas = false;
    var err = internal_classify(book.?, h, &is_atlas);
    if (err != k_ra8_ok) {
        return err;
    }
    if (is_atlas) {
        return epub_tile_binder_add(binder, book, h, image_id); // in-place
    }
    const c = cfg.?;
    var info = std.mem.zeroes(jof_info_t);
    err = internal_transcode(book.?, h, c, &info);
    if (err != k_ra8_ok) {
        return err;
    }
    return epub_tile_binder_add_ext(binder, c.store.pread, c.store.pread_ctx, @as(u64, info.total_size), image_id);
}

// ---- test-only helpers ------------------------------------------------------
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

// ---- test-only stub surface (teststub/img_import_test_stubs.c) --------------
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
