// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

//! Unit tests for the `.rabook` raster source adapter (`src/zoom_book.zig`).

const std = @import("std");
const book = @import("book");
const stubs = @import("zoom_stubs.zig");

const B = stubs.BookStubs(book);

comptime {
    _ = @import("zoom_stubs.zig");
    _ = stubs.LogSink();
    _ = B;
}

const Err = book.zoom.Err;
const Source = book.zoom.Source;
const BookImage = book.BookImage;
const BookSrc = book.BookSrc;

test "srcInit rejects null binding and null source" {
    var bs: BookSrc = std.mem.zeroes(BookSrc);
    try std.testing.expectEqual(Err.null_ptr, book.zoom_book_src_init(null, &bs, 1));
    try std.testing.expectEqual(Err.null_ptr, book.zoom_book_src_init(&bs, null, 1));
}

test "srcInit propagates the descriptor read error" {
    var bs: BookSrc = std.mem.zeroes(BookSrc);
    B.test_img_err = .no_mem;
    defer B.test_img_err = .ok;
    try std.testing.expectEqual(Err.no_mem, book.zoom_book_src_init(&bs, &bs, 1));
}

test "srcInit rejects a vector entry and a zero extent" {
    var bs: BookSrc = std.mem.zeroes(BookSrc);
    B.test_img = std.mem.zeroes(BookImage);
    B.test_img.format = 1; // k_book_image_svg
    try std.testing.expectEqual(Err.not_supported, book.zoom_book_src_init(&bs, &bs, 1));
    B.test_img.format = 0;
    try std.testing.expectEqual(Err.invalid_arg, book.zoom_book_src_init(&bs, &bs, 1));
}

test "srcInit binds a gray4 raster and copies the descriptor" {
    var bs: BookSrc = std.mem.zeroes(BookSrc);
    B.test_img = std.mem.zeroes(BookImage);
    B.test_img.width = 64;
    B.test_img.height = 32;
    try std.testing.expectEqual(Err.ok, book.zoom_book_src_init(&bs, &bs, 1));
    try std.testing.expectEqual(@as(?*const anyopaque, &bs), bs.src);
    try std.testing.expectEqual(@as(u16, 64), bs.img.width);
    try std.testing.expectEqual(@as(u16, 32), bs.img.height);
}

test "bind rejects nulls and an uninitialised binding" {
    var bs: BookSrc = std.mem.zeroes(BookSrc);
    var src: Source = std.mem.zeroes(Source);
    try std.testing.expectEqual(Err.null_ptr, book.zoom_book_src_bind(null, &src));
    try std.testing.expectEqual(Err.null_ptr, book.zoom_book_src_bind(&bs, null));
    try std.testing.expectEqual(Err.null_ptr, book.zoom_book_src_bind(&bs, &src));
}

test "bind presents the adapter as a zoom source" {
    var bs: BookSrc = std.mem.zeroes(BookSrc);
    B.test_img = std.mem.zeroes(BookImage);
    B.test_img.width = 64;
    B.test_img.height = 32;
    try std.testing.expectEqual(Err.ok, book.zoom_book_src_init(&bs, &bs, 1));
    var src: Source = std.mem.zeroes(Source);
    try std.testing.expectEqual(Err.ok, book.zoom_book_src_bind(&bs, &src));
    try std.testing.expectEqual(@as(?*const anyopaque, @ptrCast(&bs)), @as(?*const anyopaque, @ptrCast(src.ctx)));
    try std.testing.expectEqual(@as(u32, 64), src.width);
    try std.testing.expectEqual(@as(u32, 32), src.height);
    try std.testing.expect(src.read != null);
}

test "read rejects nulls and forwards the rect call" {
    var bs: BookSrc = std.mem.zeroes(BookSrc);
    var out: [8]u8 = undefined;
    try std.testing.expectEqual(Err.null_ptr, book.zoom_book_read(null, 0, 0, 1, 1, &out, 1));
    try std.testing.expectEqual(Err.null_ptr, book.zoom_book_read(&bs, 0, 0, 1, 1, null, 1));
    try std.testing.expectEqual(Err.null_ptr, book.zoom_book_read(&bs, 0, 0, 1, 1, &out, 1));

    B.test_img = std.mem.zeroes(BookImage);
    B.test_img.width = 64;
    B.test_img.height = 32;
    try std.testing.expectEqual(Err.ok, book.zoom_book_src_init(&bs, &bs, 1));
    B.test_rect_calls = 0;
    B.test_rect_err = .ok;
    try std.testing.expectEqual(Err.ok, book.zoom_book_read(&bs, 3, 5, 2, 4, &out, 8));
    try std.testing.expectEqual(@as(u32, 1), B.test_rect_calls);
    try std.testing.expectEqual(@as(u32, 3), B.test_last.x);
    try std.testing.expectEqual(@as(u32, 5), B.test_last.y);
    try std.testing.expectEqual(@as(u32, 2), B.test_last.w);
    try std.testing.expectEqual(@as(u32, 4), B.test_last.h);
    try std.testing.expectEqual(@as(u32, 8), B.test_last.stride);
}

test "read propagates the rect error verbatim" {
    var bs: BookSrc = std.mem.zeroes(BookSrc);
    var out: [8]u8 = undefined;
    B.test_img = std.mem.zeroes(BookImage);
    B.test_img.width = 64;
    B.test_img.height = 32;
    try std.testing.expectEqual(Err.ok, book.zoom_book_src_init(&bs, &bs, 1));
    B.test_rect_err = .invalid_arg;
    defer B.test_rect_err = .ok;
    try std.testing.expectEqual(Err.invalid_arg, book.zoom_book_read(&bs, 0, 0, 1, 1, &out, 1));
}
