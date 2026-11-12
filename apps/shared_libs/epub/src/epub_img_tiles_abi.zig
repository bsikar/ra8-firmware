//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Tile binder + reflow <img> loader (RA8FW-1194), ported from
//! src/epub_img_tiles.c behind the unchanged inc/epub_img_tiles.h contract.
//! Semantics and error paths match the C TU exactly: the binder registers up
//! to k_epub_tile_max_sources atlas images against one caller-supplied tile
//! cache (book-backed sources read through epub_entry_pread, external sources
//! through their own pread seam), decode-on-miss reads one JOF tile via
//! jof_read_tile with the budget mapping invalid_size -> no_mem, and the
//! reflow <img> loader resolves an href into a caller-owned bounded scratch
//! through epub_get_resource. The C TU's
//! __has_include("ra8_tile_cache.h") && __has_include("jof.h") emptiness
//! contract moves to the build: this TU ships as its own archive
//! (ra8_epub_img_tiles) whose members are pulled only when a consumer calls
//! the binder or the loader, so an epub-only app that never links jof keeps
//! the same link surface the empty C translation unit gave it. The C TU's
//! per-argument log lines are dropped here, as in the other abi slices; the
//! returned codes are identical.

const std = @import("std");

// ---- ra8_err_t (libs/ra8_core/inc/ra8_err.h, C23 enum : uint16_t) -----------
const ra8_err_t = u16;
const k_ra8_ok: ra8_err_t = 0;
const k_ra8_err_no_mem: ra8_err_t = 0x102;
const k_ra8_err_invalid_arg: ra8_err_t = 0x103;
const k_ra8_err_invalid_size: ra8_err_t = 0x105;
const k_ra8_err_not_found: ra8_err_t = 0x106;
const k_ra8_err_out_of_range: ra8_err_t = 0x208;
const k_ra8_err_null_ptr: ra8_err_t = 0x504;

// ---- epub.h / epub_img_tiles.h sizes ----------------------------------------
const k_epub_max_path_len = 192;
const k_epub_tile_max_sources = 8;

const epub_book_t = opaque {};

// ---- jof.h (exact C layout) --------------------------------------------------
const jof_pread_fn = *const fn (?*anyopaque, u64, ?[*]u8, usize, ?*usize) callconv(.c) ra8_err_t;

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

// ---- ra8_keycache.h (exact C layout; the tile cache embeds one) --------------
const ra8_keycache_hash_fn = *const fn (?*const anyopaque, u32, ?*anyopaque) callconv(.c) u32;
const ra8_keycache_render_fn = *const fn (?*anyopaque, ?*const anyopaque, ?[*]u8, u32, ?*anyopaque) callconv(.c) ra8_err_t;

/// ra8_keycache_evict_t (C23 enum : uint8_t).
const ra8_keycache_evict_t = u8;

/// ra8_keycache_cell_t: per-cell link metadata.
const ra8_keycache_cell_t = extern struct {
    prev: i32,
    next: i32,
    hash_next: i32,
    pin_count: u16,
    seg: u8,
    valid: u8,
};

/// ra8_keycache_cfg_t: caller-supplied storage + policy (copied at init).
const ra8_keycache_cfg_t = extern struct {
    cell_mem: ?[*]u8,
    cell_bytes: u32,
    cell_count: u32,
    key_mem: ?[*]u8,
    key_bytes: u32,
    user_mem: ?[*]u8,
    user_bytes: u32,
    meta: ?[*]ra8_keycache_cell_t,
    buckets: ?[*]i32,
    bucket_count: u32,
    render: ?ra8_keycache_render_fn,
    render_ctx: ?*anyopaque,
    evict: ra8_keycache_evict_t,
    protected_pct: u8,
    hash: ?ra8_keycache_hash_fn,
    hash_ctx: ?*anyopaque,
};

/// ra8_keycache_t: the underlying keyed-LRU cache engine.
const ra8_keycache_t = extern struct {
    cfg: ra8_keycache_cfg_t,
    pb_head: i32,
    pb_tail: i32,
    pt_head: i32,
    pt_tail: i32,
    protected_count: u32,
    protected_cap: u32,
    hits: u32,
    misses: u32,
    evictions: u32,
};

// ---- ra8_tile_cache.h (exact C layout) ---------------------------------------
/// ra8_tile_key_t: padding-free 12 bytes (byte-wise key comparison).
const ra8_tile_key_t = extern struct {
    image_id: u32,
    tile_x: u16,
    tile_y: u16,
    zoom: u16,
    reserved: u16,
};

/// ra8_tile_dims_t: decoded tile dimensions.
const ra8_tile_dims_t = extern struct {
    w: u16,
    h: u16,
};

/// ra8_tile_t: a pinned view of a cached tile.
const ra8_tile_t = extern struct {
    pixels: ?[*]const u8,
    width: u16,
    height: u16,
};

const ra8_tile_decode_fn = ?*const fn (?*anyopaque, ?*const ra8_tile_key_t, ?[*]u8, u32, ?*u16, ?*u16) callconv(.c) ra8_err_t;

/// ra8_tile_cache_cfg_t: caller-supplied tile-cache storage + decoder.
const ra8_tile_cache_cfg_t = extern struct {
    cell_mem: ?[*]u8,
    cell_bytes: u32,
    cell_count: u32,
    meta: ?[*]ra8_keycache_cell_t,
    keys: ?[*]ra8_tile_key_t,
    dims: ?[*]ra8_tile_dims_t,
    buckets: ?[*]i32,
    bucket_count: u32,
    decode: ra8_tile_decode_fn,
    decode_ctx: ?*anyopaque,
};

/// ra8_tile_cache_t: the tile cache the binder owns.
const ra8_tile_cache_t = extern struct {
    kc: ra8_keycache_t,
    decode: ra8_tile_decode_fn,
    decode_ctx: ?*anyopaque,
};

/// ra8_tile_pan_dir_t (C23 enum : uint8_t).
const ra8_tile_pan_dir_t = u8;

/// ra8_tile_rect_t: inclusive tile-column/row viewport bounds.
const ra8_tile_rect_t = extern struct {
    tx0: u16,
    ty0: u16,
    tx1: u16,
    ty1: u16,
};

/// ra8_tile_prefetch_req_t: one pan-prefetch request.
const ra8_tile_prefetch_req_t = extern struct {
    image_id: u32,
    view: ra8_tile_rect_t,
    tile_cols: u16,
    tile_rows: u16,
    zoom: u16,
    max_tiles: u16,
    dir: ra8_tile_pan_dir_t,
};

// ---- epub_entry.h (exact C layout) --------------------------------------------
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

// ---- epub_img_tiles.h (exact C layout) ----------------------------------------
/// epub_tile_source_t: one registered atlas image (private to the binder).
const epub_tile_source_t = extern struct {
    book: ?*epub_book_t,
    path: [k_epub_max_path_len]u8,
    pread: ?jof_pread_fn,
    pread_ctx: ?*anyopaque,
    total_size: u64,
    image_id: u32,
    info: jof_info_t,
};

/// epub_tile_binder_t: binds up to k_epub_tile_max_sources images to one
/// tile cache, decode-on-miss wired to this module's JOF tile reader.
const epub_tile_binder_t = extern struct {
    cache: ra8_tile_cache_t,
    sources: [k_epub_tile_max_sources]epub_tile_source_t,
    source_count: u8,
    scratch: ?[*]u8,
    scratch_cap: u32,
};

/// epub_img_loader_t: context for the real reflow <img> loader.
const epub_img_loader_t = extern struct {
    book: ?*epub_book_t,
    scratch: ?[*]u8,
    scratch_cap: usize,
};

// ---- extern surface (resolved at the final link) ------------------------------
extern fn epub_entry_open(book: ?*epub_book_t, path: ?[*:0]const u8, out_reader: ?*epub_entry_reader_t, out_size: ?*u64) callconv(.c) ra8_err_t;
extern fn epub_entry_close(reader: ?*epub_entry_reader_t) callconv(.c) ra8_err_t;
extern fn epub_entry_pread(book: ?*epub_book_t, path: ?[*:0]const u8, offset: u64, buf: ?[*]u8, len: usize, got: ?*usize) callconv(.c) ra8_err_t;
extern fn epub_get_resource(book: ?*epub_book_t, path: ?[*:0]const u8, out_buf: ?[*]u8, max_len: usize, got_len: ?*usize) callconv(.c) ra8_err_t;
extern fn jof_parse(pread: jof_pread_fn, pread_ctx: ?*anyopaque, total_size: u64, out_info: ?*jof_info_t) callconv(.c) ra8_err_t;
extern fn jof_read_tile(pread: jof_pread_fn, pread_ctx: ?*anyopaque, info: ?*const jof_info_t, tile_x: u16, tile_y: u16, scratch: ?[*]u8, scratch_cap: u32, out_px: ?[*]u8, out_cap: u32, out_w: ?*u16, out_h: ?*u16) callconv(.c) ra8_err_t;
extern fn ra8_tile_cache_init(tc: ?*ra8_tile_cache_t, cfg: ?*const ra8_tile_cache_cfg_t) callconv(.c) ra8_err_t;
extern fn ra8_tile_cache_get(tc: ?*ra8_tile_cache_t, key: ?*const ra8_tile_key_t, out_tile: ?*ra8_tile_t) callconv(.c) ra8_err_t;
extern fn ra8_tile_cache_put(tc: ?*ra8_tile_cache_t, pixels: ?[*]const u8) callconv(.c) ra8_err_t;
extern fn ra8_tile_cache_prefetch_pan(tc: ?*ra8_tile_cache_t, req: ?*const ra8_tile_prefetch_req_t, out_warmed: ?*u16) callconv(.c) ra8_err_t;

comptime {
    // The key is compared byte-wise over 12 bytes; the reader is 40 B on the
    // probed 64-bit host, 32 B under AAPCS (32-bit pointers, u64 8-aligned).
    if (@sizeOf(ra8_tile_key_t) != 12) @compileError("ra8_tile_key_t must stay padding-free at 12 bytes");
    const k_reader_size: usize = if (@sizeOf(usize) == 8) 40 else 32;
    if (@sizeOf(epub_entry_reader_t) != k_reader_size) @compileError("epub_entry_reader_t layout drift");
    if (k_epub_max_path_len != 192) @compileError("k_epub_max_path_len must match epub.h");
}

// ---------------------------------------------------------------------------
// Small helpers.
// ---------------------------------------------------------------------------

/// Find the registered source index for image_id, or -1.
/// Linear scan bounded by both the live count and the source cap.
fn internal_find(binder: *const epub_tile_binder_t, image_id: u32) i32 {
    var i: u8 = 0;
    while (i < binder.source_count and i < k_epub_tile_max_sources) : (i += 1) {
        if (binder.sources[i].image_id == image_id) {
            return @intCast(i);
        }
    }
    return -1;
}

/// jof_pread_fn adapter over a stored archive entry: ctx is the owning
/// epub_tile_source_t; the read windows the entry's uncompressed bytes via
/// epub_entry_pread.
fn internal_entry_pread(ctx: ?*anyopaque, offset: u64, buf: ?[*]u8, len: usize, got: ?*usize) callconv(.c) ra8_err_t {
    const src: *const epub_tile_source_t = @ptrCast(@alignCast(ctx.?));
    return epub_entry_pread(src.book, @ptrCast(&src.path), offset, buf, len, got);
}

/// Measure an archive entry's uncompressed size: opens (and immediately
/// closes) an entry cursor purely for the size report.
fn internal_measure_entry(book: *epub_book_t, path: [*:0]const u8, out_size: *u64) ra8_err_t {
    var reader = std.mem.zeroes(epub_entry_reader_t);
    var err = epub_entry_open(book, path, &reader, out_size);
    if (err != k_ra8_ok) {
        return err;
    }
    err = epub_entry_close(&reader);
    return err;
}

// ---------------------------------------------------------------------------
// Tile decode-on-miss.
// ---------------------------------------------------------------------------

/// Tile-cache decode-on-miss trampoline (ra8_tile_decode_fn). Resolves the
/// key's image and decodes the tile off its backing via jof_read_tile. A tile
/// or its stored stream that exceeds the cell/scratch budget maps to
/// k_ra8_err_no_mem, preserving the binder's documented contract.
fn internal_tile_decode(ctx: ?*anyopaque, key: ?*const ra8_tile_key_t, cell: ?[*]u8, cell_bytes: u32, out_w: ?*u16, out_h: ?*u16) callconv(.c) ra8_err_t {
    const binder: *epub_tile_binder_t = @ptrCast(@alignCast(ctx.?));
    const k = key.?;
    const si = internal_find(binder, k.image_id);
    if (si < 0) {
        return k_ra8_err_not_found; // get() rejects unregistered ids before the cache
    }
    const src = &binder.sources[@intCast(si)];
    const pread: jof_pread_fn = if (src.book != null) internal_entry_pread else src.pread.?;
    const pctx: ?*anyopaque = if (src.book != null) src else src.pread_ctx;
    const err = jof_read_tile(pread, pctx, &src.info, k.tile_x, k.tile_y, binder.scratch, binder.scratch_cap, cell, cell_bytes, out_w.?, out_h.?);
    if (err == k_ra8_err_invalid_size) {
        return k_ra8_err_no_mem; // tile exceeds the cell / scratch budget
    }
    return err;
}

// ---------------------------------------------------------------------------
// Public API -- tile binder.
// ---------------------------------------------------------------------------

export fn epub_tile_binder_init(binder: ?*epub_tile_binder_t, storage: ?*const ra8_tile_cache_cfg_t, scratch: ?[*]u8, scratch_cap: u32) callconv(.c) ra8_err_t {
    if (binder == null) return k_ra8_err_null_ptr;
    if (storage == null) return k_ra8_err_null_ptr;
    const b = binder.?;
    b.* = std.mem.zeroes(epub_tile_binder_t);
    b.scratch = scratch;
    b.scratch_cap = if (scratch != null) scratch_cap else 0;
    var cfg = storage.?.*;
    cfg.decode = internal_tile_decode;
    cfg.decode_ctx = b;
    return ra8_tile_cache_init(&b.cache, &cfg);
}

/// Common pre-registration guards shared by both add paths: the capacity
/// check and the duplicate-id check.
fn internal_check_slot(binder: *const epub_tile_binder_t, image_id: u32) ra8_err_t {
    if (binder.source_count >= k_epub_tile_max_sources) {
        return k_ra8_err_no_mem;
    }
    if (internal_find(binder, image_id) >= 0) {
        return k_ra8_err_invalid_arg;
    }
    return k_ra8_ok;
}

/// Fill + validate a book-backed source slot (measure, then parse). Runs
/// against the final slot address so the entry-pread adapter's ctx (the slot
/// itself) is stable; the caller commits the count only on success.
fn internal_fill_book_source(src: *epub_tile_source_t, book: *epub_book_t, path: [*:0]const u8, plen: usize, image_id: u32) ra8_err_t {
    src.book = book;
    src.image_id = image_id;
    @memcpy(src.path[0 .. plen + 1], path[0 .. plen + 1]);
    var err = internal_measure_entry(book, path, &src.total_size);
    if (err == k_ra8_ok) {
        err = jof_parse(internal_entry_pread, src, src.total_size, &src.info);
    }
    if (err != k_ra8_ok) {
        src.* = std.mem.zeroes(epub_tile_source_t);
    }
    return err;
}

export fn epub_tile_binder_add(binder: ?*epub_tile_binder_t, book: ?*epub_book_t, path: ?[*:0]const u8, image_id: u32) callconv(.c) ra8_err_t {
    if (binder == null) return k_ra8_err_null_ptr;
    if (book == null) return k_ra8_err_null_ptr;
    if (path == null) return k_ra8_err_null_ptr;
    const b = binder.?;
    const plen = std.mem.len(path.?);
    if (plen == 0 or plen >= k_epub_max_path_len) {
        return k_ra8_err_invalid_arg;
    }
    const slot_err = internal_check_slot(b, image_id);
    if (slot_err != k_ra8_ok) {
        return slot_err;
    }
    const src = &b.sources[b.source_count];
    src.* = std.mem.zeroes(epub_tile_source_t);
    const err = internal_fill_book_source(src, book.?, path.?, plen, image_id);
    if (err != k_ra8_ok) {
        return err;
    }
    b.source_count += 1;
    return k_ra8_ok;
}

export fn epub_tile_binder_add_ext(binder: ?*epub_tile_binder_t, pread: ?jof_pread_fn, pread_ctx: ?*anyopaque, total_size: u64, image_id: u32) callconv(.c) ra8_err_t {
    if (binder == null) return k_ra8_err_null_ptr;
    if (pread == null) return k_ra8_err_null_ptr;
    const b = binder.?;
    const slot_err = internal_check_slot(b, image_id);
    if (slot_err != k_ra8_ok) {
        return slot_err;
    }
    const src = &b.sources[b.source_count];
    src.* = std.mem.zeroes(epub_tile_source_t);
    src.pread = pread;
    src.pread_ctx = pread_ctx;
    src.total_size = total_size;
    src.image_id = image_id;
    const err = jof_parse(pread.?, pread_ctx, total_size, &src.info);
    if (err != k_ra8_ok) {
        src.* = std.mem.zeroes(epub_tile_source_t);
        return err;
    }
    b.source_count += 1;
    return k_ra8_ok;
}

export fn epub_tile_binder_info(binder: ?*const epub_tile_binder_t, image_id: u32, out_info: ?*jof_info_t) callconv(.c) ra8_err_t {
    if (binder == null) return k_ra8_err_null_ptr;
    if (out_info == null) return k_ra8_err_null_ptr;
    const si = internal_find(binder.?, image_id);
    if (si < 0) {
        return k_ra8_err_not_found;
    }
    out_info.?.* = binder.?.sources[@intCast(si)].info;
    return k_ra8_ok;
}

export fn epub_tile_binder_get(binder: ?*epub_tile_binder_t, image_id: u32, tile_x: u16, tile_y: u16, out_tile: ?*ra8_tile_t) callconv(.c) ra8_err_t {
    if (binder == null) return k_ra8_err_null_ptr;
    if (out_tile == null) return k_ra8_err_null_ptr;
    const b = binder.?;
    const si = internal_find(b, image_id);
    if (si < 0) {
        return k_ra8_err_not_found;
    }
    const info = &b.sources[@intCast(si)].info;
    if (tile_x >= info.tile_cols) {
        return k_ra8_err_out_of_range;
    }
    if (tile_y >= info.tile_rows) {
        return k_ra8_err_out_of_range;
    }
    const key = ra8_tile_key_t{ .image_id = image_id, .tile_x = tile_x, .tile_y = tile_y, .zoom = 0, .reserved = 0 };
    return ra8_tile_cache_get(&b.cache, &key, out_tile.?);
}

export fn epub_tile_binder_put(binder: ?*epub_tile_binder_t, pixels: ?[*]const u8) callconv(.c) ra8_err_t {
    if (binder == null) return k_ra8_err_null_ptr;
    if (pixels == null) return k_ra8_err_null_ptr;
    return ra8_tile_cache_put(&binder.?.cache, pixels);
}

export fn epub_tile_binder_prefetch_pan(binder: ?*epub_tile_binder_t, image_id: u32, view: ?*const ra8_tile_rect_t, dir: ra8_tile_pan_dir_t, max_tiles: u16, out_warmed: ?*u16) callconv(.c) ra8_err_t {
    if (binder == null) return k_ra8_err_null_ptr;
    if (view == null) return k_ra8_err_null_ptr;
    const b = binder.?;
    const si = internal_find(b, image_id);
    if (si < 0) {
        return k_ra8_err_not_found;
    }
    const info = &b.sources[@intCast(si)].info;
    const req = ra8_tile_prefetch_req_t{
        .image_id = image_id,
        .view = view.?.*,
        .tile_cols = info.tile_cols,
        .tile_rows = info.tile_rows,
        .zoom = 0,
        .max_tiles = max_tiles,
        .dir = dir,
    };
    return ra8_tile_cache_prefetch_pan(&b.cache, &req, out_warmed);
}

// ---------------------------------------------------------------------------
// Public API -- reflow img loader.
// ---------------------------------------------------------------------------

/// Validate a bound loader context + href length (single-condition guards,
/// split out to keep the public loader short, as in the C TU).
fn internal_loader_args_ok(ld: *const epub_img_loader_t, href_len: u32) ra8_err_t {
    if (ld.book == null) return k_ra8_err_invalid_arg;
    if (ld.scratch == null) return k_ra8_err_invalid_arg;
    if (ld.scratch_cap == 0) return k_ra8_err_invalid_arg;
    if (href_len == 0) return k_ra8_err_invalid_arg;
    if (href_len >= k_epub_max_path_len) return k_ra8_err_invalid_arg;
    return k_ra8_ok;
}

export fn epub_reflow_img_load(ctx: ?*anyopaque, href: ?[*]const u8, href_len: u32, out_bytes: ?*?[*]const u8, out_len: ?*usize) callconv(.c) ra8_err_t {
    if (ctx == null) return k_ra8_err_null_ptr;
    if (href == null) return k_ra8_err_null_ptr;
    if (out_bytes == null) return k_ra8_err_null_ptr;
    if (out_len == null) return k_ra8_err_null_ptr;
    out_len.?.* = 0;
    const ld: *epub_img_loader_t = @ptrCast(@alignCast(ctx.?));
    const ok = internal_loader_args_ok(ld, href_len);
    if (ok != k_ra8_ok) {
        return ok;
    }
    var path_buf: [k_epub_max_path_len]u8 = undefined;
    @memcpy(path_buf[0..href_len], href.?[0..href_len]);
    path_buf[href_len] = 0;
    const cpath: [*:0]const u8 = @ptrCast(&path_buf);
    var got: usize = 0;
    const err = epub_get_resource(ld.book.?, cpath, ld.scratch, ld.scratch_cap, &got);
    if (err != k_ra8_ok) {
        return err;
    }
    out_bytes.?.* = ld.scratch;
    out_len.?.* = got;
    return k_ra8_ok;
}

// ---------------------------------------------------------------------------
// Tests (host; the teststub C file stands in for the extern surface).
// ---------------------------------------------------------------------------

const testing = std.testing;

// Test-only hooks the stub C file provides (never linked in production).
extern fn tiles_teststub_set_tile_err(code: u16) void;
extern fn tiles_teststub_ext_pread(ctx: ?*anyopaque, offset: u64, buf: ?[*]u8, len: usize, got: ?*usize) callconv(.c) ra8_err_t;

const k_stub_atlas_total: u64 = 64;
const k_stub_payload: [4]u8 = .{ 0xDE, 0xAD, 0xBE, 0xEF };

fn stub_storage() ra8_tile_cache_cfg_t {
    return ra8_tile_cache_cfg_t{
        .cell_mem = null, // filled per-test with real backing
        .cell_bytes = 0,
        .cell_count = 0,
        .meta = null,
        .keys = null,
        .dims = null,
        .buckets = null,
        .bucket_count = 0,
        .decode = null,
        .decode_ctx = null,
    };
}

test "binder null guards" {
    var storage = stub_storage();
    var binder = std.mem.zeroes(epub_tile_binder_t);
    var tile = std.mem.zeroes(ra8_tile_t);
    var info = std.mem.zeroes(jof_info_t);
    try testing.expectEqual(k_ra8_err_null_ptr, epub_tile_binder_init(null, &storage, null, 0));
    try testing.expectEqual(k_ra8_err_null_ptr, epub_tile_binder_init(&binder, null, null, 0));
    try testing.expectEqual(k_ra8_err_null_ptr, epub_tile_binder_add(null, @ptrFromInt(1), "a.jof", 1));
    try testing.expectEqual(k_ra8_err_null_ptr, epub_tile_binder_add(&binder, null, "a.jof", 1));
    try testing.expectEqual(k_ra8_err_null_ptr, epub_tile_binder_add(&binder, @ptrFromInt(1), null, 1));
    try testing.expectEqual(k_ra8_err_null_ptr, epub_tile_binder_add_ext(null, null, null, 0, 1));
    try testing.expectEqual(k_ra8_err_null_ptr, epub_tile_binder_add_ext(&binder, null, null, 0, 1));
    try testing.expectEqual(k_ra8_err_null_ptr, epub_tile_binder_info(null, 1, &info));
    try testing.expectEqual(k_ra8_err_null_ptr, epub_tile_binder_info(&binder, 1, null));
    try testing.expectEqual(k_ra8_err_null_ptr, epub_tile_binder_get(null, 1, 0, 0, &tile));
    try testing.expectEqual(k_ra8_err_null_ptr, epub_tile_binder_get(&binder, 1, 0, 0, null));
    try testing.expectEqual(k_ra8_err_null_ptr, epub_tile_binder_put(null, @ptrFromInt(1)));
    try testing.expectEqual(k_ra8_err_null_ptr, epub_tile_binder_put(&binder, null));
    try testing.expectEqual(k_ra8_err_null_ptr, epub_tile_binder_prefetch_pan(null, 1, &ra8_tile_rect_t{ .tx0 = 0, .ty0 = 0, .tx1 = 0, .ty1 = 0 }, 0, 1, null));
    try testing.expectEqual(k_ra8_err_null_ptr, epub_reflow_img_load(null, "x", 1, null, null));
}

test "book-backed add, info, duplicate, bad path, failed measure" {
    var cell_mem: [256]u8 align(8) = undefined;
    var meta: [4]ra8_keycache_cell_t = undefined;
    var keys: [4]ra8_tile_key_t = undefined;
    var dims: [4]ra8_tile_dims_t = undefined;
    var buckets: [4]i32 = undefined;
    var scratch: [256]u8 = undefined;

    var storage = stub_storage();
    storage.cell_mem = &cell_mem;
    storage.cell_bytes = 256;
    storage.cell_count = 4;
    storage.meta = &meta;
    storage.keys = &keys;
    storage.dims = &dims;
    storage.buckets = &buckets;
    storage.bucket_count = 4;

    var binder = std.mem.zeroes(epub_tile_binder_t);
    try testing.expectEqual(k_ra8_ok, epub_tile_binder_init(&binder, &storage, &scratch, 256));
    try testing.expectEqual(@as(u8, 0), binder.source_count);

    const book: *epub_book_t = @ptrFromInt(0x1000);
    try testing.expectEqual(k_ra8_ok, epub_tile_binder_add(&binder, book, "a.jof", 7));
    try testing.expectEqual(@as(u8, 1), binder.source_count);
    try testing.expectEqual(k_stub_atlas_total, binder.sources[0].total_size);
    try testing.expectEqual(@as(u32, 7), binder.sources[0].image_id);

    var info = std.mem.zeroes(jof_info_t);
    try testing.expectEqual(k_ra8_ok, epub_tile_binder_info(&binder, 7, &info));
    try testing.expectEqual(@as(u16, 2), info.tile_cols);

    // Duplicate id is rejected.
    try testing.expectEqual(k_ra8_err_invalid_arg, epub_tile_binder_add(&binder, book, "a.jof", 7));

    // Empty and over-long paths are rejected.
    try testing.expectEqual(k_ra8_err_invalid_arg, epub_tile_binder_add(&binder, book, "", 8));
    var long_buf: [k_epub_max_path_len + 1]u8 = @splat('x'); // 192 chars: len == cap, rejected
    long_buf[k_epub_max_path_len] = 0;
    try testing.expectEqual(k_ra8_err_invalid_arg, epub_tile_binder_add(&binder, book, @ptrCast(&long_buf), 8));

    // A failed measure re-zeros the slot and frees it again.
    try testing.expectEqual(k_ra8_err_not_found, epub_tile_binder_add(&binder, book, "missing.jof", 9));
    try testing.expectEqual(@as(u8, 1), binder.source_count);
    try testing.expectEqual(@as(u32, 0), binder.sources[1].image_id);
    try testing.expectEqual(k_ra8_ok, epub_tile_binder_add(&binder, book, "a.jof", 9));
}

test "source table fills to the cap" {
    var cell_mem: [256]u8 align(8) = undefined;
    var meta: [16]ra8_keycache_cell_t = undefined;
    var keys: [16]ra8_tile_key_t = undefined;
    var dims: [16]ra8_tile_dims_t = undefined;
    var buckets: [4]i32 = undefined;
    var scratch: [256]u8 = undefined;

    var storage = stub_storage();
    storage.cell_mem = &cell_mem;
    storage.cell_bytes = 256;
    storage.cell_count = 16;
    storage.meta = &meta;
    storage.keys = &keys;
    storage.dims = &dims;
    storage.buckets = &buckets;
    storage.bucket_count = 4;

    var binder = std.mem.zeroes(epub_tile_binder_t);
    try testing.expectEqual(k_ra8_ok, epub_tile_binder_init(&binder, &storage, &scratch, 256));
    const book: *epub_book_t = @ptrFromInt(0x1000);
    var id: u32 = 1;
    while (id <= k_epub_tile_max_sources) : (id += 1) {
        try testing.expectEqual(k_ra8_ok, epub_tile_binder_add(&binder, book, "a.jof", id));
    }
    try testing.expectEqual(k_ra8_err_no_mem, epub_tile_binder_add(&binder, book, "a.jof", 100));
}

test "external add_ext and the get/put/prefetch surface" {
    var cell_mem: [256]u8 align(8) = undefined;
    var meta: [4]ra8_keycache_cell_t = undefined;
    var keys: [4]ra8_tile_key_t = undefined;
    var dims: [4]ra8_tile_dims_t = undefined;
    var buckets: [4]i32 = undefined;
    var scratch: [256]u8 = undefined;

    var storage = stub_storage();
    storage.cell_mem = &cell_mem;
    storage.cell_bytes = 256;
    storage.cell_count = 4;
    storage.meta = &meta;
    storage.keys = &keys;
    storage.dims = &dims;
    storage.buckets = &buckets;
    storage.bucket_count = 4;

    var binder = std.mem.zeroes(epub_tile_binder_t);
    try testing.expectEqual(k_ra8_ok, epub_tile_binder_init(&binder, &storage, &scratch, 256));

    // Unregistered lookups reject before the cache sees anything.
    var tile = std.mem.zeroes(ra8_tile_t);
    try testing.expectEqual(k_ra8_err_not_found, epub_tile_binder_get(&binder, 42, 0, 0, &tile));
    var miss_info = std.mem.zeroes(jof_info_t);
    try testing.expectEqual(k_ra8_err_not_found, epub_tile_binder_info(&binder, 42, &miss_info));
    var warmed: u16 = 99;
    try testing.expectEqual(k_ra8_err_not_found, epub_tile_binder_prefetch_pan(&binder, 42, &ra8_tile_rect_t{ .tx0 = 0, .ty0 = 0, .tx1 = 1, .ty1 = 1 }, 2, 4, &warmed));

    // Register an external (pread-backed) atlas.
    try testing.expectEqual(k_ra8_ok, epub_tile_binder_add_ext(&binder, &tiles_teststub_ext_pread, null, k_stub_atlas_total, 42));
    try testing.expectEqual(@as(u8, 1), binder.source_count);

    // Out-of-range tile coordinates reject before decode.
    try testing.expectEqual(k_ra8_err_out_of_range, epub_tile_binder_get(&binder, 42, 2, 0, &tile));
    try testing.expectEqual(k_ra8_err_out_of_range, epub_tile_binder_get(&binder, 42, 0, 2, &tile));

    // A valid get decodes through the trampoline.
    try testing.expectEqual(k_ra8_ok, epub_tile_binder_get(&binder, 42, 1, 1, &tile));
    try testing.expectEqual(@as(u16, 64), tile.width);
    try testing.expectEqual(@as(u16, 64), tile.height);
    try testing.expectEqual(@as(u8, 0xA5), tile.pixels.?[0]);
    try testing.expectEqual(k_ra8_ok, epub_tile_binder_put(&binder, tile.pixels.?));

    // Prefetch reports the stub's warmed count.
    try testing.expectEqual(k_ra8_ok, epub_tile_binder_prefetch_pan(&binder, 42, &ra8_tile_rect_t{ .tx0 = 0, .ty0 = 0, .tx1 = 1, .ty1 = 1 }, 2, 4, &warmed));
    try testing.expectEqual(@as(u16, 1), warmed);
}

test "decode budget mapping: invalid_size becomes no_mem" {
    var cell_mem: [256]u8 align(8) = undefined;
    var meta: [4]ra8_keycache_cell_t = undefined;
    var keys: [4]ra8_tile_key_t = undefined;
    var dims: [4]ra8_tile_dims_t = undefined;
    var buckets: [4]i32 = undefined;
    var scratch: [256]u8 = undefined;

    var storage = stub_storage();
    storage.cell_mem = &cell_mem;
    storage.cell_bytes = 256;
    storage.cell_count = 4;
    storage.meta = &meta;
    storage.keys = &keys;
    storage.dims = &dims;
    storage.buckets = &buckets;
    storage.bucket_count = 4;

    var binder = std.mem.zeroes(epub_tile_binder_t);
    try testing.expectEqual(k_ra8_ok, epub_tile_binder_init(&binder, &storage, &scratch, 256));
    try testing.expectEqual(k_ra8_ok, epub_tile_binder_add_ext(&binder, &tiles_teststub_ext_pread, null, k_stub_atlas_total, 5));

    var tile = std.mem.zeroes(ra8_tile_t);
    tiles_teststub_set_tile_err(k_ra8_err_invalid_size);
    defer tiles_teststub_set_tile_err(0);
    try testing.expectEqual(k_ra8_err_no_mem, epub_tile_binder_get(&binder, 5, 0, 0, &tile));

    // Other decode errors pass through unchanged.
    tiles_teststub_set_tile_err(k_ra8_err_not_found);
    try testing.expectEqual(k_ra8_err_not_found, epub_tile_binder_get(&binder, 5, 0, 0, &tile));
}

test "reflow img loader" {
    var scratch: [256]u8 = undefined;
    var ld = epub_img_loader_t{
        .book = @ptrFromInt(0x1000),
        .scratch = &scratch,
        .scratch_cap = 256,
    };

    var out_bytes: ?[*]const u8 = null;
    var out_len: usize = 0;

    // Null guards.
    try testing.expectEqual(k_ra8_err_null_ptr, epub_reflow_img_load(null, "a.png", 5, &out_bytes, &out_len));
    try testing.expectEqual(k_ra8_err_null_ptr, epub_reflow_img_load(&ld, null, 5, &out_bytes, &out_len));
    try testing.expectEqual(k_ra8_err_null_ptr, epub_reflow_img_load(&ld, "a.png", 5, null, &out_len));
    try testing.expectEqual(k_ra8_err_null_ptr, epub_reflow_img_load(&ld, "a.png", 5, &out_bytes, null));

    // Bad args.
    try testing.expectEqual(k_ra8_err_invalid_arg, epub_reflow_img_load(&ld, "a.png", 0, &out_bytes, &out_len));
    try testing.expectEqual(k_ra8_err_invalid_arg, epub_reflow_img_load(&ld, "a.png", k_epub_max_path_len, &out_bytes, &out_len));
    const bad_ld = epub_img_loader_t{ .book = null, .scratch = &scratch, .scratch_cap = 256 };
    try testing.expectEqual(k_ra8_err_invalid_arg, epub_reflow_img_load(@constCast(&bad_ld), "a.png", 5, &out_bytes, &out_len));
    const cap0_ld = epub_img_loader_t{ .book = @ptrFromInt(0x1000), .scratch = null, .scratch_cap = 0 };
    try testing.expectEqual(k_ra8_err_invalid_arg, epub_reflow_img_load(@constCast(&cap0_ld), "a.png", 5, &out_bytes, &out_len));

    // Happy path: the stub resource payload lands in the caller scratch.
    try testing.expectEqual(k_ra8_ok, epub_reflow_img_load(&ld, "cover.png", 9, &out_bytes, &out_len));
    try testing.expectEqual(@as(usize, k_stub_payload.len), out_len);
    try testing.expectEqualSlices(u8, &k_stub_payload, out_bytes.?[0..out_len]);
}
