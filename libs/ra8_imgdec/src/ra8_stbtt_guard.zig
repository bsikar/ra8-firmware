//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! sfnt table-directory bounds validator run before `stbtt_InitFont()`
//! (RA8FW-1240). Ported 1:1 from apps/shared_libs/reflow/src/ra8_stbtt_guard.c,
//! deleted in the same commit: a pure, heap-free big-endian sfnt parser that
//! reads only the offset table and the table directory, proving every access
//! stays inside the caller-supplied buffer, and returns a verdict without ever
//! calling into stb_truetype. The threat model lives in
//! apps/shared_libs/reflow/inc/ra8_stbtt_guard.h, which stays the C ABI
//! declaration.
//!
//! The C ABI symbol `ra8_stbtt_sfnt_dir_in_bounds` is kept for the callers that
//! declare it extern (the reflow driver seam, the epub chapter TU and the stb
//! fuzz harness) and arrives with the ra8_imgdec archive the build already
//! links, so it resolves in every link, reflow v1 or litehtml v2.

const std = @import("std");

/// sfnt offset table (header) size, bytes.
const sfnt_offset_table_bytes: u64 = 12;
/// `u16` numTables offset in the offset table.
const sfnt_num_tables_off: u64 = 4;
/// Size of one table directory record, bytes.
const sfnt_table_record_bytes: u64 = 16;
/// 4-byte table tag offset in a record.
const sfnt_record_tag_off: u64 = 0;
/// `u32` table-offset field offset in a record.
const sfnt_record_offset_off: u64 = 8;
/// `u32` table-length field offset in a record.
const sfnt_record_length_off: u64 = 12;

/// 'cmap' -- character-to-glyph mapping.
const sfnt_tag_cmap: u32 = 0x636D6170;
/// 'head' -- font header (indexToLocFormat).
const sfnt_tag_head: u32 = 0x68656164;
/// 'maxp' -- maximum profile (numGlyphs).
const sfnt_tag_maxp: u32 = 0x6D617870;

/// cmap: version(2) + numTables(2) before records.
const cmap_header_bytes: u64 = 4;
/// cmap: `u16` numTables offset in the table.
const cmap_num_tables_off: u64 = 2;
/// cmap: one encoding-record size, bytes.
const cmap_record_bytes: u64 = 8;
/// head: InitFont reads `u16` at head+50 -> +52.
const head_loc_format_end: u64 = 52;
/// maxp: InitFont reads `u16` at maxp+4 -> +6.
const maxp_num_glyphs_end: u64 = 6;

/// Read a big-endian `u16` from the two bytes the caller has proven in-bounds.
fn rdBeU16(buf: []const u8) u16 {
    return (@as(u16, buf[0]) << 8) | @as(u16, buf[1]);
}

/// Read a big-endian `u32` from the four bytes the caller has proven in-bounds.
fn rdBeU32(buf: []const u8) u32 {
    return (@as(u32, buf[0]) << 24) | (@as(u32, buf[1]) << 16) |
        (@as(u32, buf[2]) << 8) | @as(u32, buf[3]);
}

/// Prove the reads `stbtt_InitFont()` makes *inside* a known table stay
/// in-bounds, given the table's already-validated file offset.
///
/// The top-level directory guard only proves each table's declared extent
/// fits. `stbtt_InitFont_internal()` then reads fields inside three tables
/// using values it trusts from the table's own bytes -- none of which the
/// declared length constrains:
///   - cmap: `numTables = ttUSHORT(cmap + 2)`, then strides the encoding-record
///     sub-directory as `cmap + 4 + 8 * i` for `i` in `[0, numTables)`. A
///     crafted numTables (up to 65535) walks ~512 KiB past the table -- the
///     reliable out-of-bounds read this validator closes. Requires
///     `t_off + 4 + 8 * numTables <= len`.
///   - head: `ttUSHORT(head + 50)` (indexToLocFormat). Requires
///     `t_off + 52 <= len`.
///   - maxp: `ttUSHORT(maxp + 4)` (numGlyphs). Requires `t_off + 6 <= len`.
/// Any other tag needs no internal check here (its contents are read later, in
/// the glyph path, not by `stbtt_InitFont`).
fn tableInternalInBounds(buf: []const u8, tag: u32, t_off: u64) bool {
    const buf_len: u64 = buf.len;
    if (tag == sfnt_tag_cmap) {
        if (t_off + cmap_header_bytes > buf_len) {
            return false;
        }
        const sub_tables = rdBeU16(buf[@intCast(t_off + cmap_num_tables_off)..][0..2]);
        const sub_end = t_off + cmap_header_bytes +
            (@as(u64, sub_tables) * cmap_record_bytes);
        return sub_end <= buf_len;
    }
    if (tag == sfnt_tag_head) {
        return (t_off + head_loc_format_end) <= buf_len;
    }
    if (tag == sfnt_tag_maxp) {
        return (t_off + maxp_num_glyphs_end) <= buf_len;
    }
    return true;
}

/// Verify that a font's sfnt table directory lies within its buffer. Mirrors
/// exactly the reads `stbtt_InitFont()` / `stbtt__find_table()` make while
/// walking the sfnt offset table, table directory, and the table-internal
/// fields InitFont reads, and confirms each one stays inside `[0, len)`.
/// See apps/shared_libs/reflow/inc/ra8_stbtt_guard.h for the full contract.
pub export fn ra8_stbtt_sfnt_dir_in_bounds(
    data: ?[*]const u8,
    len: usize,
    fontstart: u32,
) callconv(.c) bool {
    const buf = if (data) |ptr| ptr[0..len] else return false;
    const buf_len: u64 = len;
    const start: u64 = fontstart;

    // (1) The 12-byte offset table must lie within the buffer before we may
    // read numTables from it. Computed in u64 so a hostile fontstart (up to
    // UINT32_MAX) cannot wrap the sum.
    if ((start + sfnt_offset_table_bytes) > buf_len) {
        return false;
    }
    const num_tables = rdBeU16(buf[@intCast(start + sfnt_num_tables_off)..][0..2]);

    // (2) The whole table directory (num_tables records of 16 bytes) must fit.
    // num_tables is a u16 (<= 65535), so the product is bounded.
    const dir_end = start + sfnt_offset_table_bytes +
        (@as(u64, num_tables) * sfnt_table_record_bytes);
    if (dir_end > buf_len) {
        return false;
    }

    // (3) Every table's declared [offset, offset + length) extent must fit, and
    // (4) the reads stbtt_InitFont makes INSIDE cmap / head / maxp must also
    // stay in-bounds -- the top-level extent alone does not bound cmap's
    // internal 8 * numTables sub-directory walk. Both record fields are u32, so
    // the sum is at most 0x1FFFFFFFE and cannot overflow u64. The record-header
    // reads at rec+0 / rec+8 / rec+12 are proven in-bounds by check (2) above
    // (rec + 16 <= dir_end <= len).
    var i: u32 = 0;
    while (i < num_tables) : (i += 1) {
        const rec = start + sfnt_offset_table_bytes +
            (@as(u64, i) * sfnt_table_record_bytes);
        const tag = rdBeU32(buf[@intCast(rec + sfnt_record_tag_off)..][0..4]);
        const t_off: u64 = rdBeU32(buf[@intCast(rec + sfnt_record_offset_off)..][0..4]);
        const t_len: u64 = rdBeU32(buf[@intCast(rec + sfnt_record_length_off)..][0..4]);
        if ((t_off + t_len) > buf_len) {
            return false;
        }
        if (!tableInternalInBounds(buf, tag, t_off)) {
            return false;
        }
    }

    return true;
}
