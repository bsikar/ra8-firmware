//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for `epub_chapter_abi.zig`.

const std = @import("std");
const testing = std.testing;
const sut = @import("epub_chapter_abi");
const priv_epub_join_path = sut.priv_epub_join_path;
const epub_get_chapter_count = sut.epub_get_chapter_count;
const epub_load_chapter = sut.epub_load_chapter;
const epub_get_toc_kind = sut.epub_get_toc_kind;
const epub_get_toc_count = sut.epub_get_toc_count;
const epub_get_toc_entry = sut.epub_get_toc_entry;
const epub_toc_entry_to_chapter = sut.epub_toc_entry_to_chapter;
const epub_get_metadata = sut.epub_get_metadata;
const epub_get_cover_image = sut.epub_get_cover_image;
const epub_get_resource = sut.epub_get_resource;
const epub_get_embedded_font_count = sut.epub_get_embedded_font_count;
const epub_manifest_count = sut.epub_manifest_count;
const epub_manifest_item = sut.epub_manifest_item;
const epub_get_embedded_font = sut.epub_get_embedded_font;
const epub_set_font = sut.epub_set_font;
const epub_render_glyph = sut.epub_render_glyph;

const epub_book_t = sut.epub_book_t;
const epub_metadata_t = sut.epub_metadata_t;
const epub_toc_entry_t = sut.epub_toc_entry_t;
const k_ra8_err_invalid_arg = sut.k_ra8_err_invalid_arg;
const k_ra8_err_invalid_size = sut.k_ra8_err_invalid_size;
const k_ra8_err_no_mem = sut.k_ra8_err_no_mem;
const k_ra8_err_not_found = sut.k_ra8_err_not_found;
const k_ra8_err_not_initialized = sut.k_ra8_err_not_initialized;
const k_ra8_err_null_ptr = sut.k_ra8_err_null_ptr;
const k_ra8_err_out_of_range = sut.k_ra8_err_out_of_range;
const k_ra8_err_validation_failed = sut.k_ra8_err_validation_failed;
const k_ra8_ok = sut.k_ra8_ok;

extern var teststub_locate_miss_prefixed: c_int;

extern var teststub_fail_stat: c_int;

extern var teststub_guard_result: c_int;

extern var teststub_fail_extract: c_int;

extern var teststub_entry_uncomp: u64;

extern var teststub_font_offset: c_int;

extern var teststub_fail_sfnt_dir: c_int;

extern var teststub_fail_font_init: c_int;

extern var teststub_box_x0: c_int;

extern var teststub_box_y0: c_int;

extern var teststub_box_x1: c_int;

extern var teststub_box_y1: c_int;

extern fn teststub_located_prefixed() [*:0]const u8;

extern fn teststub_located_bare() [*:0]const u8;

extern fn teststub_extract_len() usize;

extern fn teststub_extract_buf() [*]const u8;

extern fn teststub_locate_calls() c_int;

extern fn teststub_scale() f32;

extern fn teststub_make_stride() c_int;

extern fn teststub_make_codepoint() c_int;

extern fn teststub_reset() void;

fn setStr(comptime N: usize, buf: *[N]u8, s: []const u8) void {
    @memcpy(buf[0..s.len], s);
    buf[s.len] = 0;
}

fn freshBook() epub_book_t {
    teststub_reset();
    var book = std.mem.zeroes(epub_book_t);
    book.in_use = 1;
    book.zip_archive_active = 1;
    book.chapter_count = 2;
    setStr(192, &book.chapter_paths[0], "ch1.xhtml");
    setStr(192, &book.chapter_paths[1], "ch2.xhtml");
    setStr(192, &book.opf_dir, "oebps/");
    setStr(192, &book.cover_path, "cover.png");
    setStr(192, &book.embedded_font_paths[0], "fonts/a.ttf");
    book.embedded_font_count = 1;
    book.manifest_count = 1;
    setStr(64, &book.manifest[0].id, "id1");
    setStr(192, &book.manifest[0].href, "ch1.xhtml");
    book.toc_count = 1;
    setStr(192, &book.toc[0].href, "ch1.xhtml#sec2");
    book.toc_kind = 2;
    return book;
}

fn freshBuf() [256]u8 {
    return @splat(0);
}

test "join_path concatenates dir and name" {
    var out: [32]u8 = undefined;
    priv_epub_join_path("oebps/", "ch1.xhtml", &out, out.len);
    try testing.expectEqualStrings("oebps/ch1.xhtml", std.mem.sliceTo(&out, 0));
}

test "join_path truncates to cap-1 and NUL-terminates" {
    var out: [8]u8 = undefined;
    priv_epub_join_path("oebps/", "ch1.xhtml", &out, out.len);
    try testing.expectEqualStrings("oebps/c", std.mem.sliceTo(&out, 0));
}

test "join_path with null dir writes name only" {
    var out: [16]u8 = undefined;
    priv_epub_join_path(null, "x.html", &out, out.len);
    try testing.expectEqualStrings("x.html", std.mem.sliceTo(&out, 0));
}

test "chapter_count ok, null and not-initialized paths" {
    var book = freshBook();
    var n: u16 = 0;
    try testing.expectEqual(k_ra8_ok, epub_get_chapter_count(&book, &n));
    try testing.expectEqual(@as(u16, 2), n);
    try testing.expectEqual(k_ra8_err_null_ptr, epub_get_chapter_count(null, &n));
    book.in_use = 0;
    try testing.expectEqual(k_ra8_err_not_initialized, epub_get_chapter_count(&book, &n));
}

test "load_chapter happy path joins opf_dir, extracts and sets got_len" {
    var book = freshBook();
    var buf = freshBuf();
    var got: usize = 0;
    teststub_entry_uncomp = 5;
    try testing.expectEqual(k_ra8_ok, epub_load_chapter(&book, 0, &buf, buf.len, &got));
    try testing.expectEqualStrings("oebps/ch1.xhtml", std.mem.sliceTo(teststub_located_prefixed(), 0));
    try testing.expectEqual(@as(usize, 5), got);
    try testing.expectEqual(@as(usize, 5), teststub_extract_len());
    try testing.expectEqual(@as(c_int, 1), teststub_locate_calls());
}

test "load_chapter falls back to the bare path on a prefixed miss" {
    var book = freshBook();
    var buf = freshBuf();
    var got: usize = 0;
    teststub_locate_miss_prefixed = 1;
    defer teststub_locate_miss_prefixed = 0;
    try testing.expectEqual(k_ra8_ok, epub_load_chapter(&book, 1, &buf, buf.len, &got));
    try testing.expectEqualStrings("ch2.xhtml", std.mem.sliceTo(teststub_located_bare(), 0));
    try testing.expectEqual(@as(c_int, 2), teststub_locate_calls());
}

test "load_chapter rejects a not-ready book, zero max_len and an out-of-range index" {
    var book = freshBook();
    var buf = freshBuf();
    var got: usize = 9;
    book.zip_archive_active = 0;
    try testing.expectEqual(k_ra8_err_not_initialized, epub_load_chapter(&book, 0, &buf, buf.len, &got));
    try testing.expectEqual(@as(usize, 0), got);
    book.zip_archive_active = 1;
    try testing.expectEqual(k_ra8_err_invalid_size, epub_load_chapter(&book, 0, &buf, 0, &got));
    try testing.expectEqual(k_ra8_err_out_of_range, epub_load_chapter(&book, 2, &buf, buf.len, &got));
    try testing.expectEqual(k_ra8_err_null_ptr, epub_load_chapter(&book, 0, null, buf.len, &got));
}

test "load_chapter propagates the guard verdict before inflation" {
    var book = freshBook();
    var buf = freshBuf();
    var got: usize = 0;
    teststub_guard_result = 0x501;
    defer teststub_guard_result = 0;
    try testing.expectEqual(k_ra8_err_validation_failed, epub_load_chapter(&book, 0, &buf, buf.len, &got));
    try testing.expectEqual(@as(usize, 0), got);
}

test "load_chapter rejects an entry larger than the caller buffer" {
    var book = freshBook();
    var buf = freshBuf();
    var got: usize = 0;
    teststub_entry_uncomp = 1000;
    defer teststub_entry_uncomp = 16;
    try testing.expectEqual(k_ra8_err_no_mem, epub_load_chapter(&book, 0, &buf, buf.len, &got));
}

test "load_chapter maps an extract failure to validation_failed" {
    var book = freshBook();
    var buf = freshBuf();
    var got: usize = 0;
    teststub_fail_extract = 1;
    defer teststub_fail_extract = 0;
    try testing.expectEqual(k_ra8_err_validation_failed, epub_load_chapter(&book, 0, &buf, buf.len, &got));
}

test "toc accessors: kind, count, entry and out-of-range" {
    var book = freshBook();
    var kind: u8 = 0;
    try testing.expectEqual(k_ra8_ok, epub_get_toc_kind(&book, &kind));
    try testing.expectEqual(@as(u8, 2), kind);
    var n: u16 = 0;
    try testing.expectEqual(k_ra8_ok, epub_get_toc_count(&book, &n));
    try testing.expectEqual(@as(u16, 1), n);
    var entry: epub_toc_entry_t = undefined;
    try testing.expectEqual(k_ra8_ok, epub_get_toc_entry(&book, 0, &entry));
    try testing.expectEqualStrings("ch1.xhtml#sec2", std.mem.sliceTo(&entry.href, 0));
    try testing.expectEqual(k_ra8_err_out_of_range, epub_get_toc_entry(&book, 1, &entry));
}

test "toc_entry_to_chapter strips the fragment and finds the spine index" {
    var book = freshBook();
    var idx: u16 = 0;
    try testing.expectEqual(k_ra8_ok, epub_toc_entry_to_chapter(&book, 0, &idx));
    try testing.expectEqual(@as(u16, 0), idx);
    setStr(192, &book.toc[0].href, "missing.xhtml");
    try testing.expectEqual(k_ra8_err_not_found, epub_toc_entry_to_chapter(&book, 0, &idx));
    try testing.expectEqual(k_ra8_err_out_of_range, epub_toc_entry_to_chapter(&book, 1, &idx));
}

test "metadata returns pointers into the book" {
    var book = freshBook();
    setStr(128, &book.title, "A Book");
    setStr(128, &book.author, "An Author");
    var meta: epub_metadata_t = undefined;
    try testing.expectEqual(k_ra8_ok, epub_get_metadata(&book, &meta));
    try testing.expectEqualStrings("A Book", std.mem.sliceTo(meta.title.?, 0));
    try testing.expectEqualStrings("An Author", std.mem.sliceTo(meta.author.?, 0));
    book.in_use = 0;
    try testing.expectEqual(k_ra8_err_not_initialized, epub_get_metadata(&book, &meta));
}

test "cover_image: empty cover path is not-found, real path extracts" {
    var book = freshBook();
    var buf = freshBuf();
    var got: usize = 0;
    teststub_entry_uncomp = 32;
    try testing.expectEqual(k_ra8_ok, epub_get_cover_image(&book, &buf, buf.len, &got));
    try testing.expectEqual(@as(usize, 32), got);
    book.cover_path[0] = 0;
    try testing.expectEqual(k_ra8_err_not_found, epub_get_cover_image(&book, &buf, buf.len, &got));
}

test "resource extracts by the caller path" {
    var book = freshBook();
    var buf = freshBuf();
    var got: usize = 0;
    teststub_entry_uncomp = 9;
    try testing.expectEqual(k_ra8_ok, epub_get_resource(&book, "css/main.css", &buf, buf.len, &got));
    try testing.expectEqualStrings("oebps/css/main.css", std.mem.sliceTo(teststub_located_prefixed(), 0));
    try testing.expectEqual(k_ra8_err_invalid_size, epub_get_resource(&book, "css/main.css", &buf, 0, &got));
}

test "manifest accessors: count, item pointer, not-ready and out-of-range" {
    var book = freshBook();
    try testing.expectEqual(@as(u16, 1), epub_manifest_count(&book));
    const item = epub_manifest_item(&book, 0).?;
    try testing.expectEqualStrings("id1", std.mem.sliceTo(&item.id, 0));
    try testing.expect(epub_manifest_item(&book, 1) == null);
    book.in_use = 0;
    try testing.expectEqual(@as(u16, 0), epub_manifest_count(&book));
    try testing.expect(epub_manifest_item(&book, 0) == null);
}

test "embedded font: count, extract and out-of-range" {
    var book = freshBook();
    var n: u16 = 0;
    try testing.expectEqual(k_ra8_ok, epub_get_embedded_font_count(&book, &n));
    try testing.expectEqual(@as(u16, 1), n);
    var buf = freshBuf();
    var got: usize = 0;
    teststub_entry_uncomp = 128;
    try testing.expectEqual(k_ra8_ok, epub_get_embedded_font(&book, 0, &buf, buf.len, &got));
    try testing.expectEqual(@as(usize, 128), got);
    try testing.expectEqual(k_ra8_err_out_of_range, epub_get_embedded_font(&book, 1, &buf, buf.len, &got));
}

test "set_font rejects a tiny blob and accepts a real one" {
    var book = freshBook();
    var blob = freshBuf();
    blob[0] = 0x00;
    try testing.expectEqual(k_ra8_err_invalid_size, epub_set_font(&book, &blob, 8));
    try testing.expectEqual(k_ra8_ok, epub_set_font(&book, &blob, blob.len));
    try testing.expect(book.font_data != null);
    try testing.expectEqual(@as(usize, 256), book.font_size);
    book.in_use = 0;
    try testing.expectEqual(k_ra8_err_not_initialized, epub_set_font(&book, &blob, blob.len));
}

test "render_glyph: not-init, bad size, sfnt-dir and reversed-box rejects" {
    var book = freshBook();
    var blob = freshBuf();
    var bmp = freshBuf();
    var w: u32 = 0;
    var h: u32 = 0;
    try testing.expectEqual(k_ra8_err_not_initialized, epub_render_glyph(&book, 65, 12, &bmp, bmp.len, &w, &h));
    try testing.expectEqual(@as(u32, 0), w);
    try testing.expectEqual(k_ra8_ok, epub_set_font(&book, &blob, blob.len));
    try testing.expectEqual(k_ra8_err_invalid_arg, epub_render_glyph(&book, 65, 0, &bmp, bmp.len, &w, &h));
    teststub_fail_sfnt_dir = 1;
    try testing.expectEqual(k_ra8_err_validation_failed, epub_render_glyph(&book, 65, 12, &bmp, bmp.len, &w, &h));
    teststub_fail_sfnt_dir = 0;
    teststub_box_x0 = 4;
    teststub_box_x1 = 2;
    try testing.expectEqual(k_ra8_err_validation_failed, epub_render_glyph(&book, 65, 12, &bmp, bmp.len, &w, &h));
    teststub_box_x0 = 0;
    teststub_box_x1 = 8;
}

test "render_glyph rejects a bitmap over the pixel budget and renders under it" {
    var book = freshBook();
    var blob = freshBuf();
    var bmp = freshBuf();
    var w: u32 = 0;
    var h: u32 = 0;
    try testing.expectEqual(k_ra8_ok, epub_set_font(&book, &blob, blob.len));
    try testing.expectEqual(k_ra8_err_no_mem, epub_render_glyph(&book, 65, 12, &bmp, 7, &w, &h));
    try testing.expectEqual(k_ra8_ok, epub_render_glyph(&book, 65, 12, &bmp, bmp.len, &w, &h));
    try testing.expectEqual(@as(u32, 8), w);
    try testing.expectEqual(@as(u32, 16), h);
    try testing.expectEqual(@as(c_int, 8), teststub_make_stride());
    try testing.expectEqual(@as(c_int, 65), teststub_make_codepoint());
    try testing.expect(teststub_scale() > 0.0);
}
