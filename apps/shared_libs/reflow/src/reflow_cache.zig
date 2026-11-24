// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

//! Import-time pagination cache serialiser/loader for the reflow engine
//! (src/reflow_cache.c, RA8FW-1230).
//!
//! Zig port of the pagination cache surface declared in reflow_cache.h:
//! turn a laid-out engine's `glyphs[]` + `pages[]` into a keyed
//! little-endian byte blob and back, so the expensive
//! `reflow_layout_chapter()` pass can be skipped when the viewport /
//! font / colours / font blob / chapter bytes are unchanged.
//!
//! The serialisation is explicit per field (no struct memcpy), which
//! keeps the format compiler-independent and keeps the body checksum
//! deterministic because padding bytes are never hashed. Every loop is
//! bounded by a count the loader first checks against the engine's
//! static pools (`k_reflow_max_glyphs` / `k_reflow_max_pages`) and the
//! blob length, so a malformed blob can never overrun. The wire format
//! must stay byte-exact so blobs written by earlier builds keep loading;
//! the encode/decode round-trip is the verification contract.
//!
//! The exports keep C linkage so the C callers (the corpus round-trip
//! test suite) link against them exactly as before.
//!
//! @since 0.1.0

const std = @import("std");
const abi = @import("reflow_parse_abi.zig");

pub const ReflowEngine = abi.ReflowEngine;
pub const ReflowGlyph = abi.ReflowGlyph;
pub const ReflowPage = abi.ReflowPage;

// ---- Error codes (libs/ra8_core/inc/ra8_err.h; the C ABI keeps ra8_err_t
// ---- a plain c_int, so these mirror the C enumerators) ----

pub const k_ra8_ok: c_int = 0;
pub const k_ra8_err_invalid_arg: c_int = 0x103;
pub const k_ra8_err_invalid_state: c_int = 0x104;
pub const k_ra8_err_invalid_size: c_int = 0x105;
pub const k_ra8_err_not_found: c_int = 0x106;
pub const k_ra8_err_not_initialized: c_int = 0x10F;
pub const k_ra8_err_null_ptr: c_int = 0x504;
pub const k_ra8_err_checksum_mismatch: c_int = 0x502;

// ---- Format identity + layout (inc/reflow_cache.h) ----

/// Blob magic ('R''F''C''1'), stored little-endian.
pub const k_reflow_cache_magic: u32 = 0x52464331;
/// Serialised format version; bumped on any layout change.
pub const k_reflow_cache_version: u16 = 2;
/// Fixed header size, bytes.
pub const k_reflow_cache_header_bytes: usize = 52;
/// Serialised bytes per glyph (4+4+4+4+2+1+1).
pub const k_reflow_cache_glyph_bytes: usize = 20;
/// Serialised bytes per page (two u32 index fields).
pub const k_reflow_cache_page_bytes: usize = 8;

// ---- Engine static pools (mirror of reflow_parse_abi.zig) ----

const k_reflow_max_glyphs: u32 = 32768;
const k_reflow_max_pages: u32 = 256;

// ---- Internal constants (no magic numbers) ----

/// FNV-1a 32-bit offset basis.
pub const k_priv_fnv_offset: u32 = 0x811C9DC5;
/// FNV-1a 32-bit prime.
const k_priv_fnv_prime: u32 = 0x01000193;
/// Low-byte mask.
const k_priv_byte_mask: u32 = 0xFF;
/// Body-checksum field offset in the serialised header.
const k_priv_checksum_off: usize = 48;

/// Parsed header / invalidation key of a cache blob (priv_cache_key_t).
const CacheKey = struct {
    magic: u32 = 0,
    version: u16 = 0,
    viewport_w: u16 = 0,
    viewport_h: u16 = 0,
    font_px: u16 = 0,
    body_color: u32 = 0,
    link_color: u32 = 0,
    font_len: u32 = 0,
    font_hash: u32 = 0,
    content_len: u32 = 0,
    content_hash: u32 = 0,
    glyph_count: u32 = 0,
    page_count: u32 = 0,
    body_checksum: u32 = 0,
};

// ===========================================================================
// Little-endian byte cursors
// ===========================================================================

/// Append a little-endian u32 and advance the cursor. Caller pre-sizes
/// the buffer, so there is no bounds check (mirrors internal_put_u32).
fn internal_put_u32(buf: [*]u8, off: *usize, word: u32) void {
    buf[off.*] = @truncate(word);
    off.* += 1;
    buf[off.*] = @truncate(word >> 8);
    off.* += 1;
    buf[off.*] = @truncate(word >> 16);
    off.* += 1;
    buf[off.*] = @truncate(word >> 24);
    off.* += 1;
}

/// Append a little-endian u16 and advance the cursor.
fn internal_put_u16(buf: [*]u8, off: *usize, half: u16) void {
    buf[off.*] = @truncate(half);
    off.* += 1;
    buf[off.*] = @truncate(half >> 8);
    off.* += 1;
}

/// Read a little-endian u32 and advance the cursor. Caller pre-validates.
fn internal_get_u32(buf: [*]const u8, off: *usize) u32 {
    var word: u32 = buf[off.*];
    off.* += 1;
    word |= @as(u32, buf[off.*]) << 8;
    off.* += 1;
    word |= @as(u32, buf[off.*]) << 16;
    off.* += 1;
    word |= @as(u32, buf[off.*]) << 24;
    off.* += 1;
    return word;
}

/// Read a little-endian u16 and advance the cursor.
fn internal_get_u16(buf: [*]const u8, off: *usize) u16 {
    var half: u16 = buf[off.*];
    off.* += 1;
    half |= @as(u16, buf[off.*]) << 8;
    off.* += 1;
    return half;
}

// ===========================================================================
// Hash + record codecs
// ===========================================================================

/// 32-bit FNV-1a over a byte range; len == 0 yields the offset basis.
/// Used for the font hash, the content hash and the body checksum.
pub fn internal_fnv1a(data: ?[*]const u8, len: usize) u32 {
    var hash: u32 = k_priv_fnv_offset;
    if (data == null) {
        return hash;
    }
    var i: usize = 0;
    while (i < len) : (i += 1) {
        hash ^= data.?[i];
        hash *%= k_priv_fnv_prime;
    }
    return hash;
}

/// Serialise one glyph (20 bytes) and advance the cursor. `reserved`
/// carries the 1-based <a> link id (layout state, not padding), so it
/// round-trips.
fn internal_put_glyph(buf: [*]u8, off: *usize, glyph: *const ReflowGlyph) void {
    internal_put_u32(buf, off, @bitCast(glyph.x));
    internal_put_u32(buf, off, @bitCast(glyph.y));
    internal_put_u32(buf, off, @bitCast(glyph.cp));
    internal_put_u32(buf, off, glyph.color);
    internal_put_u16(buf, off, glyph.font_px);
    buf[off.*] = glyph.style;
    off.* += 1;
    buf[off.*] = glyph.reserved;
    off.* += 1;
}

/// Deserialise one glyph (20 bytes) and advance the cursor. No field is
/// silently zeroed: the restored struct is byte-identical.
fn internal_get_glyph(buf: [*]const u8, off: *usize, glyph: *ReflowGlyph) void {
    glyph.x = @bitCast(internal_get_u32(buf, off));
    glyph.y = @bitCast(internal_get_u32(buf, off));
    glyph.cp = @bitCast(internal_get_u32(buf, off));
    glyph.color = internal_get_u32(buf, off);
    glyph.font_px = internal_get_u16(buf, off);
    glyph.style = buf[off.*];
    off.* += 1;
    glyph.reserved = buf[off.*];
    off.* += 1;
}

/// Serialise one page record (8 bytes) and advance the cursor.
fn internal_put_page(buf: [*]u8, off: *usize, page: *const ReflowPage) void {
    internal_put_u32(buf, off, page.glyph_first);
    internal_put_u32(buf, off, page.glyph_count);
}

/// Deserialise one page record (8 bytes) and advance the cursor.
fn internal_get_page(buf: [*]const u8, off: *usize, page: *ReflowPage) void {
    page.glyph_first = internal_get_u32(buf, off);
    page.glyph_count = internal_get_u32(buf, off);
}

// ===========================================================================
// Header codecs + key compare
// ===========================================================================

/// Hash the engine's font blob; 0 when no font is bound. Shared by
/// serialise and key-compare so both see the same value.
fn internal_font_hash(engine: *const ReflowEngine) u32 {
    if (engine.font_data == null) {
        return 0;
    }
    return internal_fnv1a(engine.font_data, engine.font_len);
}

/// Write the fixed-size header (checksum left as a zero placeholder,
/// back-patched by the serialiser once the payload exists).
fn internal_put_header(
    buf: [*]u8,
    engine: *const ReflowEngine,
    content_len: usize,
    content_hash: u32,
) void {
    var off: usize = 0;
    internal_put_u32(buf, &off, k_reflow_cache_magic);
    internal_put_u16(buf, &off, k_reflow_cache_version);
    internal_put_u16(buf, &off, 0); // reserved
    internal_put_u16(buf, &off, engine.viewport_w);
    internal_put_u16(buf, &off, engine.viewport_h);
    internal_put_u16(buf, &off, engine.font_px);
    internal_put_u16(buf, &off, 0); // reserved16
    internal_put_u32(buf, &off, engine.body_color);
    internal_put_u32(buf, &off, engine.link_color);
    internal_put_u32(buf, &off, @truncate(engine.font_len));
    internal_put_u32(buf, &off, internal_font_hash(engine));
    internal_put_u32(buf, &off, @truncate(content_len));
    internal_put_u32(buf, &off, content_hash);
    internal_put_u32(buf, &off, engine.glyph_count);
    internal_put_u32(buf, &off, engine.page_count);
    internal_put_u32(buf, &off, 0); // body_checksum placeholder
}

/// Parse the fixed-size header into a key struct, same field order as
/// internal_put_header, skipping the two reserved half-words.
fn internal_get_header(buf: [*]const u8, key: *CacheKey) void {
    var off: usize = 0;
    key.magic = internal_get_u32(buf, &off);
    key.version = internal_get_u16(buf, &off);
    _ = internal_get_u16(buf, &off); // reserved
    key.viewport_w = internal_get_u16(buf, &off);
    key.viewport_h = internal_get_u16(buf, &off);
    key.font_px = internal_get_u16(buf, &off);
    _ = internal_get_u16(buf, &off); // reserved16
    key.body_color = internal_get_u32(buf, &off);
    key.link_color = internal_get_u32(buf, &off);
    key.font_len = internal_get_u32(buf, &off);
    key.font_hash = internal_get_u32(buf, &off);
    key.content_len = internal_get_u32(buf, &off);
    key.content_hash = internal_get_u32(buf, &off);
    key.glyph_count = internal_get_u32(buf, &off);
    key.page_count = internal_get_u32(buf, &off);
    key.body_checksum = internal_get_u32(buf, &off);
}

/// Total serialised size for the given record counts:
/// header + glyph_count * glyph_bytes + page_count * page_bytes.
fn internal_blob_size(glyph_count: u32, page_count: u32) usize {
    return k_reflow_cache_header_bytes +
        (@as(usize, glyph_count) * k_reflow_cache_glyph_bytes) +
        (@as(usize, page_count) * k_reflow_cache_page_bytes);
}

/// True when the blob's key matches the engine + supplied content. One
/// simple decision per attribute: any single mismatch makes the cached
/// layout stale.
fn internal_key_matches(
    engine: *const ReflowEngine,
    key: *const CacheKey,
    content: ?[*]const u8,
    content_len: usize,
) bool {
    if (key.viewport_w != engine.viewport_w) {
        return false;
    }
    if (key.viewport_h != engine.viewport_h) {
        return false;
    }
    if (key.font_px != engine.font_px) {
        return false;
    }
    if (key.body_color != engine.body_color) {
        return false;
    }
    if (key.link_color != engine.link_color) {
        return false;
    }
    if (key.font_len != @as(u32, @truncate(engine.font_len))) {
        return false;
    }
    if (key.font_hash != internal_font_hash(engine)) {
        return false;
    }
    if (key.content_len != @as(u32, @truncate(content_len))) {
        return false;
    }
    if (key.content_hash != internal_fnv1a(content, content_len)) {
        return false;
    }
    return true;
}

/// Validate a blob's header structure and report its total size, in
/// order: length covers the header, magic and version match, record
/// counts sit inside the engine pools, and len covers the computed
/// size. Pure structural validation: the invalidation key and the body
/// checksum are verified by the caller.
fn internal_parse_header(
    buf: [*]const u8,
    len: usize,
    key: *CacheKey,
    need: *usize,
) c_int {
    if (len < k_reflow_cache_header_bytes) {
        return k_ra8_err_invalid_size;
    }
    internal_get_header(buf, key);
    if (key.magic != k_reflow_cache_magic) {
        return k_ra8_err_not_found;
    }
    if (key.version != k_reflow_cache_version) {
        return k_ra8_err_invalid_state;
    }
    if (key.glyph_count > k_reflow_max_glyphs) {
        return k_ra8_err_invalid_size;
    }
    if (key.page_count > k_reflow_max_pages) {
        return k_ra8_err_invalid_size;
    }
    need.* = internal_blob_size(key.glyph_count, key.page_count);
    if (len < need.*) {
        return k_ra8_err_invalid_size;
    }
    return k_ra8_ok;
}

// ===========================================================================
// Public API (C linkage, inc/reflow_cache.h)
// ===========================================================================

/// Shared serialize/load precheck: init state, content args, and the
/// embedded-font multi-face cache-bypass invariant. Per-glyph embedded
/// face indices are not part of the cache key/format, so a book with any
/// registered @font-face is never cached: it live-layouts instead, and a
/// persisted page can never be mis-served under a different face set.
/// The caller checks its own NULL pointers first.
fn internal_cache_precheck(
    engine: *const ReflowEngine,
    content: ?[*]const u8,
    content_len: usize,
) c_int {
    if (engine.in_use == 0) {
        return k_ra8_err_not_initialized;
    }
    if (content == null and content_len != 0) {
        return k_ra8_err_invalid_arg;
    }
    if (engine.face_count != 0) {
        return k_ra8_err_invalid_state;
    }
    return k_ra8_ok;
}

pub export fn reflow_cache_size(
    engine: ?*const ReflowEngine,
    out_bytes: ?*usize,
) callconv(.c) c_int {
    if (engine == null or out_bytes == null) {
        return k_ra8_err_null_ptr;
    }
    if (engine.?.in_use == 0) {
        return k_ra8_err_not_initialized;
    }
    out_bytes.?.* = internal_blob_size(engine.?.glyph_count, engine.?.page_count);
    return k_ra8_ok;
}

pub export fn reflow_cache_serialize(
    engine: ?*const ReflowEngine,
    content: ?[*]const u8,
    content_len: usize,
    out_buf: ?[*]u8,
    out_cap: usize,
    out_len: ?*usize,
) callconv(.c) c_int {
    if (engine == null or out_buf == null or out_len == null) {
        return k_ra8_err_null_ptr;
    }
    const verr = internal_cache_precheck(engine.?, content, content_len);
    if (verr != k_ra8_ok) {
        return verr;
    }
    const need = internal_blob_size(engine.?.glyph_count, engine.?.page_count);
    if (out_cap < need) {
        return k_ra8_err_invalid_size;
    }

    internal_put_header(out_buf.?, engine.?, content_len, internal_fnv1a(content, content_len));

    var off: usize = k_reflow_cache_header_bytes;
    var i: u32 = 0;
    while (i < engine.?.glyph_count) : (i += 1) {
        internal_put_glyph(out_buf.?, &off, &engine.?.glyphs[i]);
    }
    i = 0;
    while (i < engine.?.page_count) : (i += 1) {
        internal_put_page(out_buf.?, &off, &engine.?.pages[i]);
    }

    const checksum = internal_fnv1a(
        out_buf.? + k_reflow_cache_header_bytes,
        need - k_reflow_cache_header_bytes,
    );
    var ck_off: usize = k_priv_checksum_off;
    internal_put_u32(out_buf.?, &ck_off, checksum);
    out_len.?.* = need;
    return k_ra8_ok;
}

pub export fn reflow_cache_load(
    engine: ?*ReflowEngine,
    content: ?[*]const u8,
    content_len: usize,
    buf: ?[*]const u8,
    len: usize,
) callconv(.c) c_int {
    if (engine == null or buf == null) {
        return k_ra8_err_null_ptr;
    }
    const verr = internal_cache_precheck(engine.?, content, content_len);
    if (verr != k_ra8_ok) {
        return verr;
    }

    var key: CacheKey = .{};
    var need: usize = 0;
    const perr = internal_parse_header(buf.?, len, &key, &need);
    if (perr != k_ra8_ok) {
        return perr;
    }
    if (!internal_key_matches(engine.?, &key, content, content_len)) {
        return k_ra8_err_invalid_state;
    }

    const checksum = internal_fnv1a(
        buf.? + k_reflow_cache_header_bytes,
        need - k_reflow_cache_header_bytes,
    );
    if (checksum != key.body_checksum) {
        return k_ra8_err_checksum_mismatch;
    }

    var off: usize = k_reflow_cache_header_bytes;
    var i: u32 = 0;
    while (i < key.glyph_count) : (i += 1) {
        internal_get_glyph(buf.?, &off, &engine.?.glyphs[i]);
    }
    i = 0;
    while (i < key.page_count) : (i += 1) {
        internal_get_page(buf.?, &off, &engine.?.pages[i]);
    }
    engine.?.glyph_count = key.glyph_count;
    engine.?.page_count = key.page_count;
    engine.?.xhtml_buf = content;
    engine.?.xhtml_len = content_len;
    return k_ra8_ok;
}
