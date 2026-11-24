//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for `epub_img_tiles_abi.zig`.

const std = @import("std");
const testing = std.testing;
const sut = @import("epub_img_tiles_abi");
const epub_tile_binder_init = sut.epub_tile_binder_init;
const epub_tile_binder_add = sut.epub_tile_binder_add;
const epub_tile_binder_add_ext = sut.epub_tile_binder_add_ext;
const epub_tile_binder_info = sut.epub_tile_binder_info;
const epub_tile_binder_get = sut.epub_tile_binder_get;
const epub_tile_binder_put = sut.epub_tile_binder_put;
const epub_tile_binder_prefetch_pan = sut.epub_tile_binder_prefetch_pan;
const epub_reflow_img_load = sut.epub_reflow_img_load;

const epub_book_t = sut.epub_book_t;
const epub_img_loader_t = sut.epub_img_loader_t;
const epub_tile_binder_t = sut.epub_tile_binder_t;
const jof_info_t = sut.jof_info_t;
const k_epub_max_path_len = sut.k_epub_max_path_len;
const k_epub_tile_max_sources = sut.k_epub_tile_max_sources;
const k_ra8_err_invalid_arg = sut.k_ra8_err_invalid_arg;
const k_ra8_err_invalid_size = sut.k_ra8_err_invalid_size;
const k_ra8_err_no_mem = sut.k_ra8_err_no_mem;
const k_ra8_err_not_found = sut.k_ra8_err_not_found;
const k_ra8_err_null_ptr = sut.k_ra8_err_null_ptr;
const k_ra8_err_out_of_range = sut.k_ra8_err_out_of_range;
const k_ra8_ok = sut.k_ra8_ok;
const ra8_err_t = sut.ra8_err_t;
const ra8_keycache_cell_t = sut.ra8_keycache_cell_t;
const ra8_tile_cache_cfg_t = sut.ra8_tile_cache_cfg_t;
const ra8_tile_dims_t = sut.ra8_tile_dims_t;
const ra8_tile_key_t = sut.ra8_tile_key_t;
const ra8_tile_rect_t = sut.ra8_tile_rect_t;
const ra8_tile_t = sut.ra8_tile_t;

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
