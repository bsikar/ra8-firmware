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
pub const ra8_err_t = u16;
pub const k_ra8_ok: ra8_err_t = 0;
pub const k_ra8_err_invalid_arg: ra8_err_t = 0x103;
pub const k_ra8_err_not_found: ra8_err_t = 0x106;
pub const k_ra8_err_not_supported: ra8_err_t = 0x107;
pub const k_ra8_err_null_ptr: ra8_err_t = 0x504;

// ---- epub.h / epub_img_tiles.h / epub_entry.h (exact C layout) --------------
const k_epub_max_path_len = 192;

/// Import-path sniffing constants (epub_import_const_t).
pub const k_epub_import_magic_len = 4;

pub const epub_book_t = opaque {};
pub const epub_tile_binder_t = opaque {};

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
pub const epub_atlas_import_cfg_t = extern struct {
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
pub const epub_entry_pull_t = extern struct {
    reader: epub_entry_reader_t,
};

pub fn internal_entry_pull(ctx: ?*anyopaque, buf: ?[*]u8, cap: usize, got: ?*usize) callconv(.c) ra8_err_t {
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
