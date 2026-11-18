//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Resident `.rabook` container: integrity/bounds validation and open
//! (RA8FW-1251). Ported 1:1 from apps/shared_libs/book/src/book.c, deleted
//! in the same commit -- this was the last C translation unit in the book
//! package, so the package is now pure Zig. Walking a validated blob is
//! pure offset arithmetic and lives in the header accessors each ported
//! TU mirrors; the only non-inline part was integrity work: the CRC-32
//! trailer check, the overflow-safe table-extent checks, and the "RBKC"
//! chunk-container open (parse, inflate, validate, publish).
//!
//! Five C ABI symbols stay exported (book_validate, book_open,
//! priv_book_crc32_extend, priv_book_container_header_fields,
//! priv_book_container_table_entry) for the C test suites and every C
//! caller. priv_book_crc32_extend and the two container-header seams are
//! the shared seams book_stream.zig and book_chunked.zig externed while
//! this TU was still C; they now resolve inside the Zig archive.

const std = @import("std");
const wire = @import("book_stream_wire.zig");

// ---- ra8_err_t (libs/ra8_core/inc/ra8_err.h, enum : uint16_t) --------------
const ra8_err_t = u16;
const k_ra8_ok: ra8_err_t = 0;
const k_ra8_err_invalid_arg: ra8_err_t = 0x103;
const k_ra8_err_invalid_size: ra8_err_t = 0x105;
const k_ra8_err_range_check_failed: ra8_err_t = 0x503;
const k_ra8_err_null_ptr: ra8_err_t = 0x504;

/// Fixed byte lengths (inc/book.h).
const k_book_sizeof_header: u32 = 100;
const k_book_sizeof_chapter: u32 = 12;
const k_book_sizeof_node: u32 = 24;
const k_book_sizeof_attr: u32 = 8;
const k_book_sizeof_stylesheet: u32 = 8;
const k_book_sizeof_image: u32 = 24;
const k_book_container_magic_len: u32 = 4;
const k_book_container_header_len: u32 = 24;
const k_book_container_entry_len: u32 = 8;

/// "RBKC" container header field offsets (book_internal.h).
const k_book_cont_off_chunk_bytes: u32 = 4;
const k_book_cont_off_total: u32 = 8;
const k_book_cont_off_count: u32 = 16;
const k_book_cont_off_reserved: u32 = 20;

/// Blob feature gates (inc/book.h).
const k_book_format_version: u32 = 1;
const k_book_flag_mask_known: u32 = 0x00000001;
const k_book_pixfmt_gray8: u8 = 1;

/// Log tag for validation diagnostics (book.c's s_tag_book).
const s_tag_book = "book";

/// Reflected CRC-32/ISO-HDLC constants (book.c's book_crc_const_t); matches
/// Python zlib.crc32 so a blob produced by tools/epub_compile verifies
/// bit-for-bit on device.
const k_book_crc_init: u32 = 0xFFFFFFFF;
const k_book_crc_poly: u32 = 0xEDB88320;

/// Host-order mirror of `book_image_t` (inc/book.h).
const BookImage = extern struct {
    id_off: u32,
    width: u16,
    height: u16,
    format: u8,
    pixel_format: u8,
    reserved2: u16,
    data_off: u32,
    data_size: u32,
    raw_size: u32,

    comptime {
        if (@sizeOf(@This()) != k_book_sizeof_image) {
            @compileError("book_image_t layout drift");
        }
    }
};

/// Caller-supplied DEFLATE inflater (book.h's book_inflate_fn).
pub const InflateFn = *const fn (
    src: [*]const u8,
    src_len: usize,
    dst: [*]u8,
    dst_cap: usize,
    out_len: *usize,
) callconv(.c) ra8_err_t;

// ---- CRC-32 ----------------------------------------------------------------

/// Extend a reflected CRC-32 over one byte span (book.c priv_book_crc32_extend).
/// Seeds with `crc ^ k_book_crc_init`, folds each byte through the reflected
/// polynomial, XORs the final value with k_book_crc_init again. Check value
/// over "123456789" is 0xCBF43926.
pub export fn priv_book_crc32_extend(crc: u32, data: [*]const u8, len: usize) callconv(.c) u32 {
    var state: u32 = crc ^ k_book_crc_init;
    var i: usize = 0;
    while (i < len) : (i += 1) {
        state ^= data[i];
        var bit: u8 = 0;
        while (bit < 8) : (bit += 1) {
            const mask: u32 = if ((state & 1) != 0) k_book_crc_init else 0;
            state = (state >> 1) ^ (k_book_crc_poly & mask);
        }
    }
    return state ^ k_book_crc_init;
}

/// CRC-32/ISO-HDLC over one resident byte span (book.c internal_crc32).
fn internalCrc32(data: []const u8) u32 {
    return priv_book_crc32_extend(0, data.ptr, data.len);
}

// ---- Resident blob validation ----------------------------------------------

/// Overflow-safe extent check (book.c internal_table_fits): true when the
/// half-open range [off, off + count * elem) lies within `total` bytes. Both
/// the start offset and the computed end are promoted to 64-bit before
/// comparison so no 32-bit arithmetic can wrap on adversarial blob fields.
fn internalTableFits(off: u32, count: u32, elem: u32, total: u32) bool {
    const end: u64 = @as(u64, off) + (@as(u64, count) * @as(u64, elem));
    return (off <= total) and (end <= @as(u64, total));
}

/// Whether every image descriptor declares a pixel format this build knows
/// (book.c internal_image_pixfmts_known): fail-closed on an unknown depth
/// rather than feed it to the wrong blit.
fn internalImagePixfmtsKnown(base: [*]const u8, hdr: wire.BookHeader) bool {
    const imgs: [*]const BookImage = @ptrCast(@alignCast(base + hdr.image_off));
    var known = true;
    var i: u32 = 0;
    while (i < hdr.image_count) : (i += 1) { // bound: validated image_count
        if (imgs[i].pixel_format > k_book_pixfmt_gray8) {
            known = false;
        }
    }
    return known;
}

/// Whether the header's magic field equals the "RABOOK1" tag (book.c
/// internal_magic_ok): all 8 bytes (7 chars + NUL).
fn internalMagicOk(hdr: *const wire.BookHeader) bool {
    const expect = "RABOOK1\x00";
    for (expect, 0..) |c, i| {
        if (hdr.magic[i] != c) {
            return false;
        }
    }
    return true;
}

/// Validate a resident `.rabook` blob (book.c book_validate): magic, layout
/// revision, known feature bits, header extent, every table's bounds
/// (64-bit-safe), known pixel depths, and the CRC-32 trailer.
pub export fn book_validate(base: ?*const anyopaque, size: usize) callconv(.c) ra8_err_t {
    if (base == null) return k_ra8_err_null_ptr; // validate: null base

    if (size < k_book_sizeof_header) {
        return k_ra8_err_invalid_size;
    }

    const hdr: *const wire.BookHeader = @ptrCast(@alignCast(base.?));

    if (!internalMagicOk(hdr)) {
        return k_ra8_err_invalid_arg;
    }
    if (hdr.format_version != k_book_format_version) {
        return k_ra8_err_invalid_arg;
    }
    // Unknown feature bits mean the blob relies on a presentation semantic
    // this firmware does not implement -- refuse it rather than silently
    // mis-render it.
    if ((hdr.flags & ~k_book_flag_mask_known) != 0) {
        return k_ra8_err_invalid_arg;
    }

    const total: u32 = hdr.total_size;
    if ((total < k_book_sizeof_header) or (@as(usize, total) > size)) {
        return k_ra8_err_invalid_size;
    }

    const tables_ok =
        internalTableFits(hdr.chapter_off, hdr.chapter_count, k_book_sizeof_chapter, total) and
        internalTableFits(hdr.node_off, hdr.node_count, k_book_sizeof_node, total) and
        internalTableFits(hdr.attr_off, hdr.attr_count, k_book_sizeof_attr, total) and
        internalTableFits(hdr.stylesheet_off, hdr.stylesheet_count, k_book_sizeof_stylesheet, total) and
        internalTableFits(hdr.image_off, hdr.image_count, k_book_sizeof_image, total) and
        internalTableFits(hdr.string_off, hdr.string_size, 1, total) and
        internalTableFits(hdr.image_pool_off, hdr.image_pool_size, 1, total);
    if (!tables_ok) {
        return k_ra8_err_invalid_size;
    }

    // Every image descriptor must name a pixel depth this build can unpack;
    // an unknown depth is refused (fail-closed) rather than fed to the wrong
    // blit.
    const bytes: [*]const u8 = @ptrCast(base.?);
    if (!internalImagePixfmtsKnown(bytes, hdr.*)) {
        return k_ra8_err_invalid_arg;
    }

    const body_len: usize = @as(usize, total) - k_book_sizeof_header;
    if (internalCrc32(bytes[k_book_sizeof_header..][0..body_len]) != hdr.crc32_val) {
        return k_ra8_err_range_check_failed;
    }

    return k_ra8_ok;
}

// ---- "RBKC" container open --------------------------------------------------

/// Parse + validate the fixed 24-byte "RBKC" container header (book.c
/// priv_book_container_header_fields): magic, non-zero chunk size and
/// inflated total, zero reserved word, and chunk count equal to
/// ceil(total / chunk_bytes). Field decoding is memcpy-style so the header
/// buffer needs no alignment; the format is documented little-endian and the
/// target is LE, matching the memcpy semantics on device.
pub export fn priv_book_container_header_fields(
    hdr_in: ?[*]const u8,
    out_chunk_bytes: ?*u32,
    out_total: ?*u64,
    out_count: ?*u32,
) callconv(.c) ra8_err_t {
    if (hdr_in == null) return k_ra8_err_null_ptr; // hdr fields: null hdr
    if (out_chunk_bytes == null) return k_ra8_err_null_ptr; // hdr fields: null out
    const hdr = hdr_in.?;

    const cmagic = [4]u8{ 'R', 'B', 'K', 'C' };
    var i: u32 = 0;
    while (i < k_book_container_magic_len) : (i += 1) {
        if (hdr[i] != cmagic[i]) {
            return k_ra8_err_invalid_arg;
        }
    }

    const chunk_bytes = std.mem.readInt(u32, hdr[k_book_cont_off_chunk_bytes..][0..4], .little);
    const total = std.mem.readInt(u64, hdr[k_book_cont_off_total..][0..8], .little);
    const count = std.mem.readInt(u32, hdr[k_book_cont_off_count..][0..4], .little);
    const reserved = std.mem.readInt(u32, hdr[k_book_cont_off_reserved..][0..4], .little);
    if ((chunk_bytes == 0) or (total == 0) or (reserved != 0)) {
        return k_ra8_err_invalid_arg;
    }

    const want_count: u64 = (total + @as(u64, chunk_bytes) - 1) / @as(u64, chunk_bytes);
    if (@as(u64, count) != want_count) {
        return k_ra8_err_invalid_arg;
    }

    out_chunk_bytes.?.* = chunk_bytes;
    out_total.?.* = total;
    out_count.?.* = count;
    return k_ra8_ok;
}

/// Unaligned-safe chunk-table load (book.c priv_book_container_table_entry):
/// entry `idx` is the uint64 LE offset at `table + idx * 8`.
pub export fn priv_book_container_table_entry(table: [*]const u8, idx: u32) callconv(.c) u64 {
    return std.mem.readInt(u64, table[@as(usize, idx) * k_book_container_entry_len ..][0..8], .little);
}

/// Parsed, bounds-checked view of a resident "RBKC" container file (book.c's
/// book_container_view_t): every pointer aims into the caller's file buffer
/// and every extent was verified against file_len, so the inflate loop can
/// trust it without re-checking.
const ContainerView = struct {
    table: [*]const u8, // first chunk-table byte (unaligned)
    payload: [*]const u8, // first byte of the chunk zlib streams
    total: u64, // flat-blob inflated total in bytes
    payload_len: u64, // concatenated stream bytes in the file
    chunk_bytes: u32, // inflated bytes per chunk (last short)
    chunk_count: u32, // number of chunks
};

/// Parse + bounds-check a resident "RBKC" container against its file (book.c
/// internal_container_view): header via the shared seam, header + chunk table
/// fit inside file_len, inflated total fits scratch_cap, then one walk of the
/// chunk table requiring offset[0] == 0, strict monotonic growth, and
/// offset[chunk_count] == payload_len.
fn internalContainerView(
    bytes: [*]const u8,
    file_len: usize,
    scratch_cap: usize,
    view: *ContainerView,
) ra8_err_t {
    if (file_len < k_book_container_header_len) {
        return k_ra8_err_invalid_size;
    }

    const err = priv_book_container_header_fields(
        bytes,
        &view.chunk_bytes,
        &view.total,
        &view.chunk_count,
    );
    if (err != k_ra8_ok) {
        return err;
    }

    const entries: u64 = @as(u64, view.chunk_count) + 1;
    const table_bytes: u64 = entries * k_book_container_entry_len;
    if (@as(u64, file_len) < @as(u64, k_book_container_header_len) + table_bytes) {
        return k_ra8_err_invalid_size;
    }
    if (@as(u64, scratch_cap) < view.total) {
        return k_ra8_err_invalid_size;
    }

    view.table = bytes + k_book_container_header_len;
    view.payload = view.table + @as(usize, @intCast(table_bytes));
    view.payload_len = @as(u64, file_len) - k_book_container_header_len - table_bytes;

    var prev = priv_book_container_table_entry(view.table, 0);
    if (prev != 0) {
        return k_ra8_err_invalid_arg;
    }
    var i: u32 = 1;
    while (i <= view.chunk_count) : (i += 1) { // bound: validated chunk_count
        const cur = priv_book_container_table_entry(view.table, i);
        if (cur <= prev) {
            return k_ra8_err_invalid_arg;
        }
        prev = cur;
    }
    if (prev != view.payload_len) {
        return k_ra8_err_invalid_arg;
    }
    return k_ra8_ok;
}

/// Inflate every chunk of a validated container view into `scratch` (book.c
/// internal_inflate_chunks): chunk i's stream occupies
/// [payload + offset[i], payload + offset[i+1]) and must inflate to exactly
/// min(chunk_bytes, total - i * chunk_bytes), written at scratch + i * chunk_bytes.
/// A produced-length mismatch on any chunk aborts.
fn internalInflateChunks(view: *const ContainerView, inflate: InflateFn, scratch: [*]u8) ra8_err_t {
    var i: u32 = 0;
    while (i < view.chunk_count) : (i += 1) { // bound: validated chunk_count
        const off = priv_book_container_table_entry(view.table, i);
        const next = priv_book_container_table_entry(view.table, i + 1);
        const dst_off: u64 = @as(u64, i) * @as(u64, view.chunk_bytes);
        var expected: u64 = view.total - dst_off;
        if (expected > @as(u64, view.chunk_bytes)) {
            expected = view.chunk_bytes;
        }

        var produced: usize = 0;
        const err = inflate(
            view.payload + @as(usize, @intCast(off)),
            @intCast(next - off),
            scratch + @as(usize, @intCast(dst_off)),
            @intCast(expected),
            &produced,
        );
        if (err != k_ra8_ok) {
            return err;
        }
        if (produced != @as(usize, @intCast(expected))) {
            return k_ra8_err_invalid_size;
        }
    }
    return k_ra8_ok;
}

/// Parse, inflate, validate, and publish an already-guarded open (book.c
/// internal_open_body).
fn internalOpenBody(
    bytes: [*]const u8,
    file_len: usize,
    inflate: InflateFn,
    scratch: [*]u8,
    scratch_cap: usize,
    out_base: *?*const anyopaque,
    out_size: *usize,
) ra8_err_t {
    var view = ContainerView{
        .table = undefined,
        .payload = undefined,
        .total = 0,
        .payload_len = 0,
        .chunk_bytes = 0,
        .chunk_count = 0,
    };

    var err = internalContainerView(bytes, file_len, scratch_cap, &view);
    if (err != k_ra8_ok) {
        return err;
    }
    err = internalInflateChunks(&view, inflate, scratch);
    if (err != k_ra8_ok) {
        return err;
    }
    err = book_validate(@ptrCast(scratch), @intCast(view.total));
    if (err != k_ra8_ok) {
        return err;
    }
    out_base.* = @ptrCast(scratch);
    out_size.* = @intCast(view.total);
    return k_ra8_ok;
}

/// Open a resident "RBKC" container file into `scratch` and validate the
/// reassembled blob (book.c book_open).
pub export fn book_open(
    file: ?*const anyopaque,
    file_len: usize,
    inflate: ?InflateFn,
    scratch: ?*anyopaque,
    scratch_cap: usize,
    out_base: ?*?*const anyopaque,
    out_size: ?*usize,
) callconv(.c) ra8_err_t {
    if (file == null) return k_ra8_err_null_ptr; // open: null file
    if (inflate == null) return k_ra8_err_null_ptr; // open: null inflate
    if (scratch == null) return k_ra8_err_null_ptr; // open: null scratch
    if (out_base == null) return k_ra8_err_null_ptr; // open: null out_base
    if (out_size == null) return k_ra8_err_null_ptr; // open: null out_size

    return internalOpenBody(
        @ptrCast(file.?),
        file_len,
        inflate.?,
        @ptrCast(scratch.?),
        scratch_cap,
        out_base.?,
        out_size.?,
    );
}

// ---- Test hooks (book_test.zig drives the internal geometry directly) ------

pub const BookImageForTest = BookImage;
pub const TestView = ContainerView;

pub fn testableTableFits(off: u32, count: u32, elem: u32, total: u32) bool {
    return internalTableFits(off, count, elem, total);
}

pub fn testableView(bytes: [*]const u8, file_len: usize, scratch_cap: usize, view: *ContainerView) ra8_err_t {
    return internalContainerView(bytes, file_len, scratch_cap, view);
}

pub fn testableInflate(view: *const ContainerView, inflate: InflateFn, scratch: [*]u8) ra8_err_t {
    return internalInflateChunks(view, inflate, scratch);
}
