//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Strict callback-driven validation of a RABOOK1 flat blob (RA8FW-1248).
//! Ported 1:1 from apps/shared_libs/book/src/book_stream.c, deleted in the
//! same commit: the canonical wire-header and string-envelope passes come
//! from the wire module, and this file coordinates every semantic table
//! pass (metadata, chapters, DOM nodes and their forward links, attributes,
//! stylesheets, images and pool tiling) plus the body CRC, all through the
//! bounded caller scratch. Validation neither maps the source nor
//! allocates memory.
//!
//! The five C ABI symbols (book_validate_stream_strict and the four
//! priv_book_stream_validate_* seams) stay exported for the C test suites
//! that reach into them; the archive provides them wherever book_stream.c
//! used to. The adapter in book_chunked_validate.zig keeps its extern
//! declaration of book_validate_stream_strict, now resolved against this
//! archive instead of the C TU.
//!
//! priv_book_crc32_extend still lives in book.c, which stays C as the
//! shared header parser; the port externs it until that TU ports.

const std = @import("std");
pub const wire = @import("book_stream_wire.zig");

// ---- ra8_err_t (libs/ra8_core/inc/ra8_err.h, enum : uint16_t) --------------
const ra8_err_t = u16;
const k_ra8_ok: ra8_err_t = 0;
const k_ra8_err_invalid_arg: ra8_err_t = 0x103;
const k_ra8_err_invalid_size: ra8_err_t = 0x105;
const k_ra8_err_range_check_failed: ra8_err_t = 0x503;
const k_ra8_err_null_ptr: ra8_err_t = 0x504;

/// Byte length of one record (inc/book.h) and the fixed 100-byte header.
const k_book_sizeof_header: u32 = 100;
const k_book_sizeof_chapter: u32 = 12;
const k_book_sizeof_node: u32 = 24;
const k_book_sizeof_attr: u32 = 8;
const k_book_sizeof_stylesheet: u32 = 8;
const k_book_sizeof_image: u32 = 24;

/// Absent index / "applies to all chapters" (inc/book.h).
const k_book_nil: u32 = 0xFFFFFFFF;
const k_book_node_element: u8 = 0;
const k_book_node_text: u8 = 1;
const k_book_image_gray4: u8 = 0;
const k_book_image_svg: u8 = 1;
const k_book_pixfmt_gray4: u8 = 0;
const k_book_pixfmt_gray8: u8 = 1;

/// Wire offsets inside a 24-byte DOM node record
/// (src/book_stream_internal.h).
const k_stream_node_kind: usize = 0;
const k_stream_node_reserved: usize = 1;
const k_stream_node_attr_count: usize = 2;
const k_stream_node_name: usize = 4;
const k_stream_node_text: usize = 8;
const k_stream_node_first_attr: usize = 12;
const k_stream_node_first_child: usize = 16;
const k_stream_node_next_sibling: usize = 20;

/// Wire offsets inside a 24-byte image descriptor
/// (src/book_stream_internal.h).
const k_stream_image_id: usize = 0;
const k_stream_image_width: usize = 4;
const k_stream_image_height: usize = 6;
const k_stream_image_format: usize = 8;
const k_stream_image_pixfmt: usize = 9;
const k_stream_image_reserved: usize = 10;
const k_stream_image_data_off: usize = 12;
const k_stream_image_data_size: usize = 16;
const k_stream_image_raw_size: usize = 20;

const BookHeader = wire.BookHeader;
const StreamValidate = wire.StreamValidate;

/// `ra8_vsource_read_fn` (libs/ra8_mem/inc/ra8_vsource.h): the exact-read
/// seam the strict validator streams through.
const VsourceReadFn = *const fn (?*anyopaque, u64, [*]u8, u32) callconv(.c) ra8_err_t;

/// The shared incremental CRC-32/ISO-HDLC from book.c, which stays C as the
/// shared header parser (inc/book.h pins the 0xCBF43926 check value).
extern fn priv_book_crc32_extend(crc: u32, data: [*]const u8, len: usize) callconv(.c) u32;

/// Decode one little-endian 16-bit field from two bytes.
fn le16(raw: []const u8) u16 {
    return @as(u16, raw[0]) | (@as(u16, raw[1]) << 8);
}

/// Decode one little-endian 32-bit field from four bytes.
fn le32(raw: []const u8) u32 {
    return @as(u32, raw[0]) | (@as(u32, raw[1]) << 8) |
        (@as(u32, raw[2]) << 16) | (@as(u32, raw[3]) << 24);
}

/// Validate the four metadata strings and optional cover index: every
/// metadata string boundary, and any non-nil cover reference must select
/// an existing image descriptor.
pub export fn priv_book_stream_validate_metadata(
    ctx: *const StreamValidate,
) callconv(.c) ra8_err_t {
    const refs = [4]u32{
        ctx.hdr.title_off,
        ctx.hdr.author_off,
        ctx.hdr.language_off,
        ctx.hdr.identifier_off,
    };
    for (refs) |ref| {
        const err = wire.priv_book_stream_string_ref(ctx, ref);
        if (err != k_ra8_ok) {
            return err;
        }
    }
    if ((ctx.hdr.cover_image_index != k_book_nil) and
        (ctx.hdr.cover_image_index >= ctx.hdr.image_count))
    {
        return k_ra8_err_invalid_arg;
    }
    return k_ra8_ok;
}

/// Validate every chapter string and root-node index: canonical title/id
/// strings, an element root with no sibling, and unique ownership of every
/// chapter root in the scratch bitset.
fn validateChapters(ctx: *const StreamValidate) ra8_err_t {
    var rec: [k_book_sizeof_chapter]u8 = @splat(0);
    var i: u32 = 0;
    while (i < ctx.hdr.chapter_count) : (i += 1) {
        const off = @as(u64, ctx.hdr.chapter_off) +
            (@as(u64, i) * @as(u64, k_book_sizeof_chapter));
        var err = wire.priv_book_stream_read(ctx, off, &rec, k_book_sizeof_chapter);
        if (err != k_ra8_ok) {
            return err;
        }
        err = wire.priv_book_stream_string_ref(ctx, le32(rec[0..4]));
        if (err == k_ra8_ok) {
            err = wire.priv_book_stream_nonempty_string_ref(ctx, le32(rec[4..8]));
        }
        const root = le32(rec[8..12]);
        if ((err == k_ra8_ok) and (root >= ctx.hdr.node_count)) {
            err = k_ra8_err_invalid_arg;
        }
        if (err == k_ra8_ok) {
            var node: [k_book_sizeof_node]u8 = @splat(0);
            err = wire.priv_book_stream_read(
                ctx,
                @as(u64, ctx.hdr.node_off) +
                    (@as(u64, root) * @as(u64, k_book_sizeof_node)),
                &node,
                k_book_sizeof_node,
            );
            if ((err == k_ra8_ok) and
                ((node[k_stream_node_kind] != k_book_node_element) or
                    (le32(node[k_stream_node_next_sibling..][0..4]) != k_book_nil)))
            {
                err = k_ra8_err_invalid_arg;
            }
        }
        if (err == k_ra8_ok) {
            const byte = root / 8;
            const mask = @as(u8, 1) << @intCast(root % 8);
            if ((ctx.scratch.?[byte] & mask) != 0) {
                err = k_ra8_err_invalid_arg;
            } else {
                ctx.scratch.?[byte] |= mask;
            }
        }
        if (err != k_ra8_ok) {
            return err;
        }
    }
    return k_ra8_ok;
}

/// Validate one optional forward node link: nil is absent, and otherwise
/// the target must be both in range and greater than the owner, which
/// excludes cycles.
fn forwardLink(link: u32, current: u32, count: u32) ra8_err_t {
    if (link == k_book_nil) {
        return k_ra8_ok;
    }
    if ((link <= current) or (link >= count)) {
        return k_ra8_err_invalid_arg;
    }
    return k_ra8_ok;
}

/// Record one unique incoming node reference in the caller bitset: validate
/// forward-link geometry before testing and setting the corresponding
/// ownership bit; duplicate parents fail closed.
fn markForwardLink(ctx: *const StreamValidate, link: u32, current: u32) ra8_err_t {
    const err = forwardLink(link, current, ctx.hdr.node_count);
    if ((err != k_ra8_ok) or (link == k_book_nil)) {
        return err;
    }
    const byte = link / 8;
    const mask = @as(u8, 1) << @intCast(link % 8);
    if ((ctx.scratch.?[byte] & mask) != 0) {
        return k_ra8_err_invalid_arg;
    }
    ctx.scratch.?[byte] |= mask;
    return k_ra8_ok;
}

/// Validate one element node and advance canonical attribute ownership:
/// element-only fields, a non-empty name, and either no attributes or the
/// exact next contiguous attribute-table span.
pub export fn priv_book_stream_validate_element(
    ctx: *const StreamValidate,
    rec: [*]const u8,
    attr_cursor: *u32,
) callconv(.c) ra8_err_t {
    if (le32(rec[k_stream_node_text..][0..4]) != 0) {
        return k_ra8_err_invalid_arg;
    }
    const err = wire.priv_book_stream_nonempty_string_ref(
        ctx,
        le32(rec[k_stream_node_name..][0..4]),
    );
    if (err != k_ra8_ok) {
        return err;
    }
    const first = le32(rec[k_stream_node_first_attr..][0..4]);
    const count = le16(rec[k_stream_node_attr_count..][0..2]);
    if (count == 0) {
        return if (first == k_book_nil) k_ra8_ok else k_ra8_err_invalid_arg;
    }
    if ((first != attr_cursor.*) or (count > (ctx.hdr.attr_count - attr_cursor.*))) {
        return k_ra8_err_invalid_arg;
    }
    attr_cursor.* += count;
    return k_ra8_ok;
}

/// Validate one text-node invariant set: reject element-only fields on text
/// records, then validate the text string boundary through the shared
/// string-pool contract.
pub export fn priv_book_stream_validate_text(
    ctx: *const StreamValidate,
    rec: [*]const u8,
) callconv(.c) ra8_err_t {
    if ((le16(rec[k_stream_node_attr_count..][0..2]) != 0) or
        (le32(rec[k_stream_node_name..][0..4]) != 0) or
        (le32(rec[k_stream_node_first_attr..][0..4]) != k_book_nil) or
        (le32(rec[k_stream_node_first_child..][0..4]) != k_book_nil))
    {
        return k_ra8_err_invalid_arg;
    }
    return wire.priv_book_stream_string_ref(ctx, le32(rec[k_stream_node_text..][0..4]));
}

/// Validate one node record's fields and mark its forward links: dispatch
/// to the kind-specific validator (element or text), then mark the node's
/// child and sibling links so the ownership-coverage pass can prove every
/// node was reached exactly once.
fn validateOneNode(
    ctx: *const StreamValidate,
    rec: []const u8,
    attr_cursor: *u32,
    index: u32,
) ra8_err_t {
    var err = if (rec[k_stream_node_kind] == k_book_node_element)
        priv_book_stream_validate_element(ctx, rec.ptr, attr_cursor)
    else if (rec[k_stream_node_kind] == k_book_node_text)
        priv_book_stream_validate_text(ctx, rec.ptr)
    else
        k_ra8_err_invalid_arg;
    if (err == k_ra8_ok) {
        err = markForwardLink(ctx, le32(rec[k_stream_node_first_child..][0..4]), index);
    }
    if (err == k_ra8_ok) {
        err = markForwardLink(ctx, le32(rec[k_stream_node_next_sibling..][0..4]), index);
    }
    return err;
}

/// Validate every DOM node and exact attribute ownership: walk nodes once,
/// validate kind-specific fields, mark unique forward edges, and finally
/// require every node and attribute owned. Forward links make cycles
/// impossible without recursion or a visited set.
fn validateNodes(ctx: *const StreamValidate) ra8_err_t {
    var rec: [k_book_sizeof_node]u8 = @splat(0);
    var attr_cursor: u32 = 0;
    var i: u32 = 0;
    while (i < ctx.hdr.node_count) : (i += 1) {
        const off = @as(u64, ctx.hdr.node_off) +
            (@as(u64, i) * @as(u64, k_book_sizeof_node));
        var err = wire.priv_book_stream_read(ctx, off, &rec, k_book_sizeof_node);
        if (err != k_ra8_ok) {
            return err;
        }
        if (rec[k_stream_node_reserved] != 0) {
            return k_ra8_err_invalid_arg;
        }
        err = validateOneNode(ctx, &rec, &attr_cursor, i);
        if (err != k_ra8_ok) {
            return err;
        }
    }
    if (attr_cursor != ctx.hdr.attr_count) {
        return k_ra8_err_invalid_arg;
    }
    i = 0;
    while (i < ctx.hdr.node_count) : (i += 1) {
        const mask = @as(u8, 1) << @intCast(i % 8);
        if ((ctx.scratch.?[i / 8] & mask) == 0) {
            return k_ra8_err_invalid_arg;
        }
    }
    return k_ra8_ok;
}

/// Validate every attribute name/value string reference: each fixed-size
/// attribute record carries a non-empty name and an empty or non-empty
/// value at a valid boundary.
fn validateAttrs(ctx: *const StreamValidate) ra8_err_t {
    var rec: [k_book_sizeof_attr]u8 = @splat(0);
    var i: u32 = 0;
    while (i < ctx.hdr.attr_count) : (i += 1) {
        var err = wire.priv_book_stream_read(
            ctx,
            @as(u64, ctx.hdr.attr_off) + (@as(u64, i) * @as(u64, k_book_sizeof_attr)),
            &rec,
            k_book_sizeof_attr,
        );
        if (err == k_ra8_ok) {
            err = wire.priv_book_stream_nonempty_string_ref(ctx, le32(rec[0..4]));
        }
        if (err == k_ra8_ok) {
            err = wire.priv_book_stream_string_ref(ctx, le32(rec[4..8]));
        }
        if (err != k_ra8_ok) {
            return err;
        }
    }
    return k_ra8_ok;
}

/// Validate every stylesheet source and scope: each stylesheet source
/// string, with only nil or an existing chapter index as its optional
/// scope.
pub export fn priv_book_stream_validate_styles(
    ctx: *const StreamValidate,
) callconv(.c) ra8_err_t {
    var rec: [k_book_sizeof_stylesheet]u8 = @splat(0);
    var i: u32 = 0;
    while (i < ctx.hdr.stylesheet_count) : (i += 1) {
        var err = wire.priv_book_stream_read(
            ctx,
            @as(u64, ctx.hdr.stylesheet_off) +
                (@as(u64, i) * @as(u64, k_book_sizeof_stylesheet)),
            &rec,
            k_book_sizeof_stylesheet,
        );
        if (err == k_ra8_ok) {
            err = wire.priv_book_stream_string_ref(ctx, le32(rec[0..4]));
        }
        const scope = le32(rec[4..8]);
        if ((err == k_ra8_ok) and (scope != k_book_nil) and
            (scope >= ctx.hdr.chapter_count))
        {
            err = k_ra8_err_invalid_arg;
        }
        if (err != k_ra8_ok) {
            return err;
        }
    }
    return k_ra8_ok;
}

/// Validate one raster image's dimensions, depth, and exact byte count:
/// non-zero dimensions, a supported gray depth, and exact packed-pixel
/// data and raw lengths computed with 64-bit arithmetic.
fn validateRaster(rec: []const u8) ra8_err_t {
    const width: u32 = le16(rec[k_stream_image_width..][0..2]);
    const height: u32 = le16(rec[k_stream_image_height..][0..2]);
    const pixfmt = rec[k_stream_image_pixfmt];
    if ((width == 0) or (height == 0) or
        ((pixfmt != k_book_pixfmt_gray4) and (pixfmt != k_book_pixfmt_gray8)))
    {
        return k_ra8_err_invalid_arg;
    }
    const pixels = @as(u64, width) * @as(u64, height);
    const expect = if (pixfmt == k_book_pixfmt_gray4) ((pixels + 1) / 2) else pixels;
    // mcdc-deactivated: validateRaster overflow backstop; width and height
    // are decoded from 16-bit wire fields, so `pixels` is at most
    // 65535*65535 == 0xFFFE0001 and `expect` (pixels, or half of it for
    // gray4) can never exceed UINT32_MAX -- the first condition is provably
    // constant-false and no input can flip it.
    if ((expect > std.math.maxInt(u32)) or
        (@as(u64, le32(rec[k_stream_image_data_size..][0..4])) != expect) or
        (@as(u64, le32(rec[k_stream_image_raw_size..][0..4])) != expect))
    {
        return k_ra8_err_invalid_size;
    }
    return k_ra8_ok;
}

/// Validate one SVG image's zero extent/depth and raw-storage length:
/// the SVG sentinel geometry, plus a non-empty stored source whose encoded
/// and raw byte lengths are identical.
fn validateSvg(rec: []const u8) ra8_err_t {
    if ((le16(rec[k_stream_image_width..][0..2]) != 0) or
        (le16(rec[k_stream_image_height..][0..2]) != 0) or
        (rec[k_stream_image_pixfmt] != k_book_pixfmt_gray4))
    {
        return k_ra8_err_invalid_arg;
    }
    const data_size = le32(rec[k_stream_image_data_size..][0..4]);
    return if ((data_size != 0) and
        (data_size == le32(rec[k_stream_image_raw_size..][0..4])))
        k_ra8_ok
    else
        k_ra8_err_invalid_arg;
}

/// Validate every image descriptor and exact gap-free pool tiling: IDs and
/// format semantics first, then a pool cursor that rejects gaps, overlap,
/// and trailing unowned image bytes.
fn validateImages(ctx: *const StreamValidate) ra8_err_t {
    var rec: [k_book_sizeof_image]u8 = @splat(0);
    var pool_cursor: u32 = 0;
    var i: u32 = 0;
    while (i < ctx.hdr.image_count) : (i += 1) {
        var err = wire.priv_book_stream_read(
            ctx,
            @as(u64, ctx.hdr.image_off) + (@as(u64, i) * @as(u64, k_book_sizeof_image)),
            &rec,
            k_book_sizeof_image,
        );
        if (err == k_ra8_ok) {
            err = wire.priv_book_stream_nonempty_string_ref(
                ctx,
                le32(rec[k_stream_image_id..][0..4]),
            );
        }
        if ((err == k_ra8_ok) and (le16(rec[k_stream_image_reserved..][0..2]) != 0)) {
            err = k_ra8_err_invalid_arg;
        }
        if (err == k_ra8_ok) {
            if (rec[k_stream_image_format] == k_book_image_gray4) {
                err = validateRaster(&rec);
            } else if (rec[k_stream_image_format] == k_book_image_svg) {
                err = validateSvg(&rec);
            } else {
                err = k_ra8_err_invalid_arg;
            }
        }
        const data_off = le32(rec[k_stream_image_data_off..][0..4]);
        const data_size = le32(rec[k_stream_image_data_size..][0..4]);
        if ((err == k_ra8_ok) and
            ((data_off != pool_cursor) or
                (data_size > (ctx.hdr.image_pool_size - pool_cursor))))
        {
            err = k_ra8_err_invalid_size;
        }
        if (err != k_ra8_ok) {
            return err;
        }
        pool_cursor += data_size;
    }
    return if (pool_cursor == ctx.hdr.image_pool_size) k_ra8_ok else k_ra8_err_invalid_size;
}

/// Hash every body byte through the caller transfer buffer: read the body
/// in scratch-sized exact spans and extend the shared CRC convention
/// without retaining the complete source.
fn validateCrc(ctx: *const StreamValidate) ra8_err_t {
    var at: u64 = k_book_sizeof_header;
    var crc: u32 = 0;
    while (at < ctx.hdr.total_size) {
        const remain = @as(u64, ctx.hdr.total_size) - at;
        var span: u32 = ctx.scratch_cap;
        if (remain < @as(u64, span)) {
            span = @intCast(remain);
        }
        const err = wire.priv_book_stream_read(ctx, at, ctx.scratch.?, span);
        if (err != k_ra8_ok) {
            return err;
        }
        crc = priv_book_crc32_extend(crc, ctx.scratch.?, span);
        at += span;
    }
    return if (crc == ctx.hdr.crc32_val) k_ra8_ok else k_ra8_err_range_check_failed;
}

/// Run the strict passes after public argument validation: decode the
/// header, validate canonical layout and each semantic table in dependency
/// order, then verify the body CRC last. Failure returns immediately
/// without publishing an output header.
fn validateBody(ctx: *StreamValidate) ra8_err_t {
    var err = wire.priv_book_stream_read_validate_header(ctx);
    if (err == k_ra8_ok) {
        err = wire.priv_book_stream_validate_string_envelope(ctx);
    }
    if (err == k_ra8_ok) {
        err = priv_book_stream_validate_metadata(ctx);
    }
    if (err == k_ra8_ok) {
        const mark_bytes = (ctx.hdr.node_count / 8) +
            @intFromBool(ctx.hdr.node_count % 8 != 0);
        @memset(ctx.scratch.?[0..mark_bytes], 0);
    }
    if (err == k_ra8_ok) {
        err = validateChapters(ctx);
    }
    if (err == k_ra8_ok) {
        err = validateNodes(ctx);
    }
    if (err == k_ra8_ok) {
        err = validateAttrs(ctx);
    }
    if (err == k_ra8_ok) {
        err = priv_book_stream_validate_styles(ctx);
    }
    if (err == k_ra8_ok) {
        err = validateImages(ctx);
    }
    if (err == k_ra8_ok) {
        err = validateCrc(ctx);
    }
    return err;
}

/// The strict RABOOK1 validator over a flat blob: argument gates first,
/// then the full semantic pass chain over a callback-driven exact-read
/// source with the caller's bounded scratch for transfer and the DOM
/// ownership map.
pub export fn book_validate_stream_strict(
    read: ?VsourceReadFn,
    read_ctx: ?*anyopaque,
    source_size: u64,
    scratch: ?[*]u8,
    scratch_cap: u32,
    out_header: ?*BookHeader,
) callconv(.c) ra8_err_t {
    const header = out_header orelse return k_ra8_err_null_ptr;
    header.* = std.mem.zeroes(BookHeader);
    const read_fn = read orelse return k_ra8_err_null_ptr;
    const scratch_ptr = scratch orelse return k_ra8_err_null_ptr;
    if ((source_size < k_book_sizeof_header) or (scratch_cap == 0)) {
        return k_ra8_err_invalid_size;
    }
    var ctx = StreamValidate{
        .read = read_fn,
        .read_ctx = read_ctx,
        .source_size = source_size,
        .scratch = scratch_ptr,
        .scratch_cap = scratch_cap,
        .hdr = std.mem.zeroes(BookHeader),
    };
    const err = validateBody(&ctx);
    if (err == k_ra8_ok) {
        header.* = ctx.hdr;
    }
    return err;
}
