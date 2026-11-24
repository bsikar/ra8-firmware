// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

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
pub export fn zoom_book_src_init(bs: ?*BookSrc, src: ?*const anyopaque, img_idx: u32) callconv(.c) Err {
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
pub export fn zoom_book_src_bind(bs: ?*BookSrc, out: ?*Source) callconv(.c) Err {
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
pub export fn zoom_book_read(ctx: ?*anyopaque, x: u32, y: u32, w: u32, h: u32, out: ?[*]u8, out_stride: u32) callconv(.c) Err {
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
