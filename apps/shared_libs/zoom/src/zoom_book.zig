//! Zig port of the `.rabook` image-pool source adapter (`inc/zoom_book.h`,
//! RA8FW-1259). Deliberately thin, exactly like zoom_book.c was: the
//! image-pool addressing contract, the gray4 nibble parity and the paged
//! frame-by-frame reads all belong to `book_src_image_rect` and are not
//! re-implemented here. This file only decides whether an entry is
//! magnifiable at all and presents the existing call as the engine's gray8
//! sub-rect seam.
//!
//! The C TU compiled to nothing when `book_paged.h` was not reachable so an
//! app that magnifies only tiled comics never linked `book`. The Zig archive
//! keeps that property structurally: `book_src_image` /
//! `book_src_image_rect` are referenced only from `zoom_book_*` exports, so
//! a consumer that never calls them never pulls this object out of the
//! archive, and `book` stays off its link line.
//!
//! External symbols: `book_src_image` and `book_src_image_rect`
//! (book_paged.h), supplied by the `book` library at the final link, and
//! `ra8_log_emit_error` (ra8_log.h's backend entry), supplied by
//! ra8_core_hal. The standalone test build installs silent/book stubs for
//! all three; the firmware build links the real definitions.

const std = @import("std");
const builtin = @import("builtin");

pub const zoom = @import("zoom.zig");

const Err = zoom.Err;
const Source = zoom.Source;
const ReadFn = zoom.ReadFn;

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) callconv(.c) void;
extern fn book_src_image(src: ?*const anyopaque, idx: u32, out_img: ?*BookImage) callconv(.c) Err;
extern fn book_src_image_rect(src: ?*const anyopaque, img: ?*const BookImage, x: u32, y: u32, w: u32, h: u32, out: ?[*]u8, out_stride: u32) callconv(.c) Err;

/// Mirror of book_image_t (apps/shared_libs/book/inc/book.h). Only the
/// fields the adapter reads (format, width, height) are interpreted; the
/// full 24-byte layout is pinned because the adapter copies whole
/// descriptors around.
pub const BookImage = extern struct {
    id_off: u32,
    width: u16,
    height: u16,
    format: u8,
    pixel_format: u8,
    reserved2: u16,
    data_off: u32,
    data_size: u32,
    raw_size: u32,
};

comptime {
    if (@sizeOf(BookImage) != 24) {
        @compileError("BookImage layout parity with book_image_t broken (k_book_sizeof_image == 24)");
    }
    if (@offsetOf(BookSrc, "img") != @sizeOf(?*const anyopaque) or
        @sizeOf(BookSrc) != @sizeOf(?*const anyopaque) + @sizeOf(BookImage))
    {
        @compileError("BookSrc layout parity with zoom_book_src_t broken");
    }
}

/// One `.rabook` raster figure, ready to be magnified (zoom_book_src_t).
/// Holds a borrowed pointer to the book source and a *copy* of the image
/// descriptor, so a paged book never faults a frame per row for 24 bytes
/// that never change.
pub const BookSrc = extern struct {
    src: ?*const anyopaque,
    img: BookImage,
};

/// Component tag for this adapter's log lines (zoom_book.c s_tag).
const s_tag: [*:0]const u8 = "zoom_book";

fn logErr(comptime msg: [:0]const u8) void {
    ra8_log_emit_error(s_tag, msg);
}

const book_gray4: u8 = 0; // k_book_image_gray4 (book.h)

/// Look up one image in a book source and prepare it for magnification
/// (zoom_book_src_init): fails closed on a vector (SVG) entry and on a zero
/// extent; a raster is accepted at either retained depth, since
/// book_src_image_rect normalises both to gray8.
export fn zoom_book_src_init(bs: ?*BookSrc, src: ?*const anyopaque, img_idx: u32) callconv(.c) Err {
    if (bs == null) {
        logErr("binding must not be nullptr");
        return .null_ptr;
    }
    if (src == null) {
        logErr("book source must not be nullptr");
        return .null_ptr;
    }
    var img: BookImage = std.mem.zeroes(BookImage);
    const img_err = book_src_image(src, img_idx, &img);
    if (img_err != .ok) {
        logErr("image descriptor read");
        return img_err;
    }
    if (img.format != book_gray4) {
        logErr("vector images are not magnifiable by the raster viewer");
        return .not_supported;
    }
    // Decision: a raster with no extent cannot be magnified (2 conditions).
    if (img.width == 0 or img.height == 0) {
        logErr("image descriptor has a zero extent");
        return .invalid_arg;
    }
    bs.?.src = src;
    bs.?.img = img;
    return .ok;
}

/// Present a bound figure to the zoom engine as a zoom_source_t
/// (zoom_book_src_bind). The binding outlives the source: out.ctx keeps the
/// borrowed BookSrc pointer.
export fn zoom_book_src_bind(bs: ?*BookSrc, out: ?*Source) callconv(.c) Err {
    if (bs == null) {
        logErr("binding must not be nullptr");
        return .null_ptr;
    }
    if (out == null) {
        logErr("source out must not be nullptr");
        return .null_ptr;
    }
    if (bs.?.src == null) {
        logErr("binding was never initialised");
        return .null_ptr;
    }
    return zoom.sourceInit(out, zoomBookReadAdapter, bs, bs.?.img.width, bs.?.img.height);
}

/// The zoom_read_fn over a `.rabook` image pool (zoom_book_read): a pure
/// forward to book_src_image_rect, which owns every pixel-decoding rule.
export fn zoom_book_read(ctx: ?*anyopaque, x: u32, y: u32, w: u32, h: u32, out: ?[*]u8, out_stride: u32) callconv(.c) Err {
    return zoomBookReadImpl(ctx, x, y, w, h, out, out_stride);
}

/// The typed ReadFn the engine binds: out is non-optional there (the seam
/// hands a real row pointer in), so a thin adapter bridges the shapes.
fn zoomBookReadAdapter(ctx: ?*anyopaque, x: u32, y: u32, w: u32, h: u32, out: [*]u8, out_stride: u32) callconv(.c) Err {
    return zoomBookReadImpl(ctx, x, y, w, h, out, out_stride);
}

fn zoomBookReadImpl(ctx: ?*anyopaque, x: u32, y: u32, w: u32, h: u32, out: ?[*]u8, out_stride: u32) Err {
    if (ctx == null) {
        logErr("read ctx must not be nullptr");
        return .null_ptr;
    }
    if (out == null) {
        logErr("read destination must not be nullptr");
        return .null_ptr;
    }
    const bs: *const BookSrc = @ptrCast(@alignCast(ctx.?));
    if (bs.src == null) {
        logErr("binding was never initialised");
        return .null_ptr;
    }
    return book_src_image_rect(bs.src, &bs.img, x, y, w, h, out, out_stride);
}

comptime {
    if (@backingInt(Err.null_ptr) != 0x504) {
        @compileError("zoom_book null_ptr code drifted from k_ra8_err_null_ptr");
    }
}

// ---------------------------------------------------------------------------
// Tests. The stubs stand in for the book library so the suite links
// standalone (the silent log sink comes with zoom.zig's own test build); the real definitions join at the firmware
// link, exactly like zoom_abi.zig's test build.
// ---------------------------------------------------------------------------

var test_img: BookImage = std.mem.zeroes(BookImage);
var test_img_err: Err = .ok;
var test_rect_calls: u32 = 0;
var test_rect_err: Err = .ok;
var test_last: struct { x: u32 = 0, y: u32 = 0, w: u32 = 0, h: u32 = 0, stride: u32 = 0 } = .{};

fn stubBookImage(src: ?*const anyopaque, idx: u32, out_img: ?*BookImage) callconv(.c) Err {
    _ = src;
    if (out_img == null) return .null_ptr;
    out_img.?.* = test_img;
    // The index is part of the contract (img_idx selects the descriptor).
    if (idx == 0) {
        out_img.?.width = 0; // the guards test drives index 0 to fail closed
    }
    return test_img_err;
}

fn stubBookImageRect(src: ?*const anyopaque, img: ?*const BookImage, x: u32, y: u32, w: u32, h: u32, out: ?[*]u8, out_stride: u32) callconv(.c) Err {
    _ = src;
    _ = img;
    test_rect_calls += 1;
    test_last = .{ .x = x, .y = y, .w = w, .h = h, .stride = out_stride };
    if (out != null) out.?[0] = 0x80;
    return test_rect_err;
}

comptime {
    if (builtin.is_test) {
        @export(&stubBookImage, .{ .name = "book_src_image" });
        @export(&stubBookImageRect, .{ .name = "book_src_image_rect" });
    }
}

test "srcInit rejects null binding and null source" {
    var bs: BookSrc = std.mem.zeroes(BookSrc);
    try std.testing.expectEqual(Err.null_ptr, zoom_book_src_init(null, &bs, 1));
    try std.testing.expectEqual(Err.null_ptr, zoom_book_src_init(&bs, null, 1));
}

test "srcInit propagates the descriptor read error" {
    var bs: BookSrc = std.mem.zeroes(BookSrc);
    test_img_err = .no_mem;
    defer test_img_err = .ok;
    try std.testing.expectEqual(Err.no_mem, zoom_book_src_init(&bs, &bs, 1));
}

test "srcInit rejects a vector entry and a zero extent" {
    var bs: BookSrc = std.mem.zeroes(BookSrc);
    test_img = std.mem.zeroes(BookImage);
    test_img.format = 1; // k_book_image_svg
    try std.testing.expectEqual(Err.not_supported, zoom_book_src_init(&bs, &bs, 1));
    test_img.format = 0;
    try std.testing.expectEqual(Err.invalid_arg, zoom_book_src_init(&bs, &bs, 1));
}

test "srcInit binds a gray4 raster and copies the descriptor" {
    var bs: BookSrc = std.mem.zeroes(BookSrc);
    test_img = std.mem.zeroes(BookImage);
    test_img.width = 64;
    test_img.height = 32;
    try std.testing.expectEqual(Err.ok, zoom_book_src_init(&bs, &bs, 1));
    try std.testing.expectEqual(@as(?*const anyopaque, &bs), bs.src);
    try std.testing.expectEqual(@as(u16, 64), bs.img.width);
    try std.testing.expectEqual(@as(u16, 32), bs.img.height);
}

test "bind rejects nulls and an uninitialised binding" {
    var bs: BookSrc = std.mem.zeroes(BookSrc);
    var src: Source = std.mem.zeroes(Source);
    try std.testing.expectEqual(Err.null_ptr, zoom_book_src_bind(null, &src));
    try std.testing.expectEqual(Err.null_ptr, zoom_book_src_bind(&bs, null));
    try std.testing.expectEqual(Err.null_ptr, zoom_book_src_bind(&bs, &src));
}

test "bind presents the adapter as a zoom source" {
    var bs: BookSrc = std.mem.zeroes(BookSrc);
    test_img = std.mem.zeroes(BookImage);
    test_img.width = 64;
    test_img.height = 32;
    try std.testing.expectEqual(Err.ok, zoom_book_src_init(&bs, &bs, 1));
    var src: Source = std.mem.zeroes(Source);
    try std.testing.expectEqual(Err.ok, zoom_book_src_bind(&bs, &src));
    try std.testing.expectEqual(@as(?*const anyopaque, @ptrCast(&bs)), @as(?*const anyopaque, @ptrCast(src.ctx)));
    try std.testing.expectEqual(@as(u32, 64), src.width);
    try std.testing.expectEqual(@as(u32, 32), src.height);
    try std.testing.expect(src.read != null);
}

test "read rejects nulls and forwards the rect call" {
    var bs: BookSrc = std.mem.zeroes(BookSrc);
    var out: [8]u8 = undefined;
    try std.testing.expectEqual(Err.null_ptr, zoom_book_read(null, 0, 0, 1, 1, &out, 1));
    try std.testing.expectEqual(Err.null_ptr, zoom_book_read(&bs, 0, 0, 1, 1, null, 1));
    try std.testing.expectEqual(Err.null_ptr, zoom_book_read(&bs, 0, 0, 1, 1, &out, 1));

    test_img = std.mem.zeroes(BookImage);
    test_img.width = 64;
    test_img.height = 32;
    try std.testing.expectEqual(Err.ok, zoom_book_src_init(&bs, &bs, 1));
    test_rect_calls = 0;
    test_rect_err = .ok;
    try std.testing.expectEqual(Err.ok, zoom_book_read(&bs, 3, 5, 2, 4, &out, 8));
    try std.testing.expectEqual(@as(u32, 1), test_rect_calls);
    try std.testing.expectEqual(@as(u32, 3), test_last.x);
    try std.testing.expectEqual(@as(u32, 5), test_last.y);
    try std.testing.expectEqual(@as(u32, 2), test_last.w);
    try std.testing.expectEqual(@as(u32, 4), test_last.h);
    try std.testing.expectEqual(@as(u32, 8), test_last.stride);
}

test "read propagates the rect error verbatim" {
    var bs: BookSrc = std.mem.zeroes(BookSrc);
    var out: [8]u8 = undefined;
    test_img = std.mem.zeroes(BookImage);
    test_img.width = 64;
    test_img.height = 32;
    try std.testing.expectEqual(Err.ok, zoom_book_src_init(&bs, &bs, 1));
    test_rect_err = .invalid_arg;
    defer test_rect_err = .ok;
    try std.testing.expectEqual(Err.invalid_arg, zoom_book_read(&bs, 0, 0, 1, 1, &out, 1));
}
