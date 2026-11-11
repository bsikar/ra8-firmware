//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Zig implementation of the xml decode half (RA8FW-1157): the C-era
//! src/xml_decode.c ported verbatim behind the unchanged C headers in inc/.
//! The decode half is pure: canonical UTF-8 scalar validation and encoding,
//! the five predefined entities plus decimal and hex character references,
//! bounded span decoding into caller-owned storage, and the decoded
//! comparisons. No reader state is touched here; the markup-scanning half
//! (xml.c, xml_doctype.c) is still C and reaches these exports through the
//! archive, so the C callers and the C tests stay untouched.

const std = @import("std");

// ---- ra8_err_t (libs/ra8_core/inc/ra8_err.h, enum : uint32_t) --------------
const ra8_err_t = c_uint;
const k_ra8_ok: ra8_err_t = 0;
const k_ra8_err_no_mem: ra8_err_t = 0x102;
const k_ra8_err_null_ptr: ra8_err_t = 0x504;
const k_ra8_err_validation_failed: ra8_err_t = 0x501;

// ---- xml.h public types (extern layout matches the C header) ---------------
const xml_span_t = extern struct { offset: c_uint, length: c_uint };

// ---- xml_internal.h constants (priv_xml_encoding_t, enum : uint32_t) -------
const k_priv_xml_tab: u32 = 0x09;
const k_priv_xml_line_feed: u32 = 0x0A;
const k_priv_xml_carriage_return: u32 = 0x0D;
const k_priv_xml_printable_min: u32 = 0x20;
const k_priv_xml_bmp_first_max: u32 = 0xD7FF;
const k_priv_xml_bmp_second_min: u32 = 0xE000;
const k_priv_xml_bmp_second_max: u32 = 0xFFFD;
const k_priv_xml_supplementary_min: u32 = 0x10000;
const k_priv_xml_scalar_max: u32 = 0x10FFFF;
const k_priv_utf8_two_lead_min: u8 = 0xC2;
const k_priv_utf8_two_lead_max: u8 = 0xDF;
const k_priv_utf8_two_payload_mask: u8 = 0x1F;
const k_priv_utf8_three_lead_min: u8 = 0xE0;
const k_priv_utf8_three_lead_max: u8 = 0xEF;
const k_priv_utf8_three_payload_mask: u8 = 0x0F;
const k_priv_utf8_three_scalar_min: u32 = 0x800;
const k_priv_utf8_four_lead_min: u8 = 0xF0;
const k_priv_utf8_four_lead_max: u8 = 0xF4;
const k_priv_utf8_four_payload_mask: u8 = 0x07;
const k_priv_utf8_continuation_mask: u8 = 0xC0;
const k_priv_utf8_continuation_tag: u8 = 0x80;
const k_priv_utf8_scalar_mask: u8 = 0x3F;
const k_priv_utf8_two_lead_tag: u8 = 0xC0;
const k_priv_utf8_three_lead_tag: u8 = 0xE0;
const k_priv_utf8_four_lead_tag: u8 = 0xF0;
const k_priv_utf8_shift_second: u5 = 12;
const k_priv_utf8_shift_third: u5 = 18;
const k_priv_xml_decimal_base: u32 = 10;

/// Incremental decoded-byte cursor used for allocation-free comparison.
const PrivDecodeCursor = struct {
    position: usize = 0,
    end: usize = 0,
    pending: [4]u8 = .{ 0, 0, 0, 0 },
    pending_at: u8 = 0,
    pending_len: u8 = 0,
};

/// C `strlen` over the caller's NUL-terminated literal.
fn c_strlen(literal: [*c]const u8) usize {
    var i: usize = 0;
    while (literal[i] != 0) : (i += 1) {}
    return i;
}

/// XML 1.0 character test: rejects forbidden controls, surrogates, and
/// out-of-range scalars.
fn internal_xml_char(cp: u32) bool {
    return (cp == k_priv_xml_tab) or (cp == k_priv_xml_line_feed) or
        (cp == k_priv_xml_carriage_return) or
        ((cp >= k_priv_xml_printable_min) and (cp <= k_priv_xml_bmp_first_max)) or
        ((cp >= k_priv_xml_bmp_second_min) and (cp <= k_priv_xml_bmp_second_max)) or
        ((cp >= k_priv_xml_supplementary_min) and (cp <= k_priv_xml_scalar_max));
}

/// Classify one UTF-8 lead byte into its scalar width and payload. Distinguishes
/// ASCII, 2/3/4-byte lead bytes, and rejects a bare continuation byte used as a
/// lead. On failure none of the three outputs are written.
fn internal_utf8_lead(
    lead: u8,
    out_cp: *u32,
    out_used: *usize,
    out_minimum: *u32,
) ra8_err_t {
    var cp: u32 = lead;
    var used: usize = 1;
    var minimum: u32 = 0;
    if ((lead >= k_priv_utf8_two_lead_min) and (lead <= k_priv_utf8_two_lead_max)) {
        cp = lead & k_priv_utf8_two_payload_mask;
        used = 2;
        minimum = k_priv_utf8_continuation_tag;
    } else if ((lead >= k_priv_utf8_three_lead_min) and (lead <= k_priv_utf8_three_lead_max)) {
        cp = lead & k_priv_utf8_three_payload_mask;
        used = 3;
        minimum = k_priv_utf8_three_scalar_min;
    } else if ((lead >= k_priv_utf8_four_lead_min) and (lead <= k_priv_utf8_four_lead_max)) {
        cp = lead & k_priv_utf8_four_payload_mask;
        used = 4;
        minimum = k_priv_xml_supplementary_min;
    } else if (lead >= k_priv_utf8_continuation_tag) {
        return k_ra8_err_validation_failed;
    } else {
        // ASCII lead: the seeded cp/used/minimum above already describe it.
    }
    out_cp.* = cp;
    out_used.* = used;
    out_minimum.* = minimum;
    return k_ra8_ok;
}

/// Decode one canonical UTF-8 scalar: shortest form, valid continuation bytes,
/// and XML characters enforced.
fn internal_utf8_next(
    source: [*]const u8,
    end: usize,
    position: usize,
    out_cp: *u32,
    out_used: *usize,
) ra8_err_t {
    if (position >= end) {
        return k_ra8_err_validation_failed;
    }
    var cp: u32 = 0;
    var used: usize = 0;
    var minimum: u32 = 0;
    const lead_err = internal_utf8_lead(source[position], &cp, &used, &minimum);
    if (lead_err != k_ra8_ok) {
        return lead_err;
    }
    if ((position + used) > end) {
        return k_ra8_err_validation_failed;
    }
    var i: usize = 1;
    while (i < used) : (i += 1) {
        const byte = source[position + i];
        if ((byte & k_priv_utf8_continuation_mask) != k_priv_utf8_continuation_tag) {
            return k_ra8_err_validation_failed;
        }
        cp = (cp << 6) | (byte & k_priv_utf8_scalar_mask);
    }
    if ((cp < minimum) or !internal_xml_char(cp)) {
        return k_ra8_err_validation_failed;
    }
    out_cp.* = cp;
    out_used.* = used;
    return k_ra8_ok;
}

/// Check that a source-relative span is in range. Uses subtraction after
/// checking the offset to avoid overflow.
export fn priv_xml_span_valid(source_len: usize, span: xml_span_t) callconv(.c) bool {
    return (@as(usize, span.offset) <= source_len) and
        (@as(usize, span.length) <= (source_len - @as(usize, span.offset)));
}

/// Encode one valid Unicode scalar as UTF-8: the canonical one-to-four-byte
/// representation. Callers validate scalars before invoking this encoder.
fn internal_utf8(cp: u32, out: *[4]u8) usize {
    if (cp < k_priv_utf8_continuation_tag) {
        out[0] = @intCast(cp);
        return 1;
    }
    if (cp < k_priv_utf8_three_scalar_min) {
        out[0] = @intCast(k_priv_utf8_two_lead_tag | (cp >> 6));
        out[1] = @intCast(k_priv_utf8_continuation_tag | (cp & k_priv_utf8_scalar_mask));
        return 2;
    }
    if (cp < k_priv_xml_supplementary_min) {
        out[0] = @intCast(k_priv_utf8_three_lead_tag | (cp >> k_priv_utf8_shift_second));
        out[1] = @intCast(k_priv_utf8_continuation_tag | ((cp >> 6) & k_priv_utf8_scalar_mask));
        out[2] = @intCast(k_priv_utf8_continuation_tag | (cp & k_priv_utf8_scalar_mask));
        return 3;
    }
    out[0] = @intCast(k_priv_utf8_four_lead_tag | (cp >> k_priv_utf8_shift_third));
    out[1] = @intCast(k_priv_utf8_continuation_tag | ((cp >> k_priv_utf8_shift_second) & k_priv_utf8_scalar_mask));
    out[2] = @intCast(k_priv_utf8_continuation_tag | ((cp >> 6) & k_priv_utf8_scalar_mask));
    out[3] = @intCast(k_priv_utf8_continuation_tag | (cp & k_priv_utf8_scalar_mask));
    return 4;
}

/// Convert one numeric-reference digit: decimal and hexadecimal reference
/// bodies. Returns UINT32_MAX when the digit is invalid for the base.
fn internal_digit(c: u8, base: u32) u32 {
    const value: u32 = c;
    if ((c >= '0') and (c <= '9')) {
        return value - '0';
    }
    if ((base == 16) and (c >= 'a') and (c <= 'f')) {
        return (value - 'a') + k_priv_xml_decimal_base;
    }
    if ((base == 16) and (c >= 'A') and (c <= 'F')) {
        return (value - 'A') + k_priv_xml_decimal_base;
    }
    return std.math.maxInt(u32);
}

/// Compare bounded source bytes against a literal of known length, one byte at
/// a time so no essentially-character operand ever reaches memcmp.
export fn priv_xml_bytes_equal(
    source: [*c]const u8,
    offset: usize,
    literal: [*c]const u8,
    length: usize,
) callconv(.c) bool {
    var i: usize = 0;
    while (i < length) : (i += 1) {
        if (source[offset + i] != literal[i]) {
            return false;
        }
    }
    return true;
}

/// Decode one entity beginning at a bounded position: the five predefined or
/// decimal/hex numeric references only.
fn internal_entity(
    source: [*]const u8,
    end: usize,
    position: usize,
    out_cp: *u32,
    out_used: *usize,
) ra8_err_t {
    const names = [_][]const u8{ "amp;", "lt;", "gt;", "quot;", "apos;" };
    const cps = [_]u32{ '&', '<', '>', '"', '\'' };
    if ((position + 2) >= end) {
        return k_ra8_err_validation_failed;
    }
    for (names, cps) |name, named_cp| {
        const length = name.len;
        if (((position + 1 + length) <= end) and
            priv_xml_bytes_equal(source, position + 1, name.ptr, length))
        {
            out_cp.* = named_cp;
            out_used.* = length + 1;
            return k_ra8_ok;
        }
    }
    if (source[position + 1] != '#') {
        return k_ra8_err_validation_failed;
    }
    var cursor: usize = position + 2;
    var base: u32 = k_priv_xml_decimal_base;
    // mcdc-deactivated: internal_entity radix probe; the `(position + 2) >= end`
    // precheck at the top of this function has already returned for every
    // shorter span, so `cursor == position + 2` is strictly less than `end` on
    // every reachable path and the bound condition is constant-true.
    if ((cursor < end) and ((source[cursor] == 'x') or (source[cursor] == 'X'))) {
        base = 16;
        cursor += 1;
    }
    const first_digit = cursor;
    var cp: u32 = 0;
    while ((cursor < end) and (source[cursor] != ';')) {
        const digit = internal_digit(source[cursor], base);
        if ((digit >= base) or (cp > ((k_priv_xml_scalar_max - digit) / base))) {
            return k_ra8_err_validation_failed;
        }
        cp = (cp * base) + digit;
        cursor += 1;
    }
    if ((cursor == first_digit) or (cursor >= end) or !internal_xml_char(cp)) {
        return k_ra8_err_validation_failed;
    }
    out_cp.* = cp;
    out_used.* = (cursor - position) + 1;
    return k_ra8_ok;
}

/// Decode one character or entity at @p cursor and append its UTF-8 bytes to
/// @p destination, clipping (or rejecting, per @p truncate) once capacity is
/// reached. A clip NUL-terminates the destination and latches @p clipped, so
/// every later character is measured but never stored.
fn internal_decode_one(
    source: [*]const u8,
    end: usize,
    cursor: usize,
    destination: ?[*]u8,
    capacity: usize,
    truncate: bool,
    output: *usize,
    clipped: *bool,
    out_used: *usize,
) ra8_err_t {
    var bytes: [4]u8 = .{ 0, 0, 0, 0 };
    var count: usize = 0;
    var used: usize = 0;
    if (source[cursor] == '&') {
        var cp: u32 = 0;
        const err = internal_entity(source, end, cursor, &cp, &used);
        if (err != k_ra8_ok) {
            return err;
        }
        count = internal_utf8(cp, &bytes);
    } else {
        var cp: u32 = 0;
        const err = internal_utf8_next(source, end, cursor, &cp, &used);
        if (err != k_ra8_ok) {
            return err;
        }
        count = used;
        @memcpy(bytes[0..count], source[cursor .. cursor + count]);
    }
    if ((destination != null) and !clipped.* and ((output.* + count) >= capacity)) {
        if (!truncate) {
            return k_ra8_err_no_mem;
        }
        clipped.* = true;
        destination.?[output.*] = 0;
    }
    if ((destination != null) and !clipped.*) {
        @memcpy(destination.?[output.* .. output.* + count], bytes[0..count]);
    }
    if ((destination == null) or !clipped.*) {
        output.* += count;
    }
    out_used.* = used;
    return k_ra8_ok;
}

/// Decode, prefix-decode, or measure one already-bounded span. Validates the
/// complete span even when prefix output clips.
fn internal_decode(
    source: [*]const u8,
    span: xml_span_t,
    destination: ?[*]u8,
    capacity: usize,
    truncate: bool,
    out_length: *usize,
) ra8_err_t {
    var output: usize = 0;
    var clipped = false;
    const end: usize = @as(usize, span.offset) + @as(usize, span.length);
    if ((destination != null) and (capacity == 0)) {
        return k_ra8_err_no_mem;
    }
    var cursor: usize = span.offset;
    while (cursor < end) {
        var used: usize = 0;
        const err = internal_decode_one(
            source,
            end,
            cursor,
            destination,
            capacity,
            truncate,
            &output,
            &clipped,
            &used,
        );
        if (err != k_ra8_ok) {
            return err;
        }
        cursor += used;
    }
    if (destination != null) {
        destination.?[output] = 0;
    }
    out_length.* = output;
    return k_ra8_ok;
}

/// Return one entity-decoded byte from a comparison cursor. Buffers remaining
/// bytes when one entity decodes to multibyte UTF-8. Used only after complete
/// decoded-size validation.
fn internal_decoded_byte(
    source: [*]const u8,
    cursor: *PrivDecodeCursor,
    out: *u8,
) ra8_err_t {
    if (cursor.pending_at < cursor.pending_len) {
        out.* = cursor.pending[cursor.pending_at];
        cursor.pending_at += 1;
        return k_ra8_ok;
    }
    if (cursor.position >= cursor.end) {
        return k_ra8_err_validation_failed;
    }
    if (source[cursor.position] != '&') {
        out.* = source[cursor.position];
        cursor.position += 1;
        return k_ra8_ok;
    }
    var cp: u32 = 0;
    var used: usize = 0;
    const err = internal_entity(source, cursor.end, cursor.position, &cp, &used);
    if (err != k_ra8_ok) {
        return err;
    }
    cursor.position += used;
    cursor.pending_len = @intCast(internal_utf8(cp, &cursor.pending));
    cursor.pending_at = 1;
    out.* = cursor.pending[0];
    return k_ra8_ok;
}

/// Entity-decode a source span into a bounded NUL-terminated buffer.
export fn xml_decode(
    source: [*c]const u8,
    source_len: usize,
    span: xml_span_t,
    destination: [*c]u8,
    capacity: usize,
    out_length: [*c]usize,
) callconv(.c) ra8_err_t {
    if ((source == null) or (destination == null) or (out_length == null)) {
        return k_ra8_err_null_ptr;
    }
    if (!priv_xml_span_valid(source_len, span)) {
        return k_ra8_err_validation_failed;
    }
    var length: usize = 0;
    const err = internal_decode(source, span, destination, capacity, false, &length);
    out_length.* = length;
    return err;
}

/// Decode the longest complete prefix that fits the destination. The prefix
/// never splits an entity or UTF-8 sequence, and the entire encoded span is
/// still validated after output clips.
export fn xml_decode_prefix(
    source: [*c]const u8,
    source_len: usize,
    span: xml_span_t,
    destination: [*c]u8,
    capacity: usize,
    out_length: [*c]usize,
) callconv(.c) ra8_err_t {
    if ((source == null) or (destination == null) or (out_length == null)) {
        return k_ra8_err_null_ptr;
    }
    if (!priv_xml_span_valid(source_len, span)) {
        return k_ra8_err_validation_failed;
    }
    var length: usize = 0;
    const err = internal_decode(source, span, destination, capacity, true, &length);
    out_length.* = length;
    return err;
}

/// Measure the entity-decoded byte count of a bounded span.
export fn xml_decoded_size(
    source: [*c]const u8,
    source_len: usize,
    span: xml_span_t,
    out_length: [*c]usize,
) callconv(.c) ra8_err_t {
    if ((source == null) or (out_length == null)) {
        return k_ra8_err_null_ptr;
    }
    if (!priv_xml_span_valid(source_len, span)) {
        return k_ra8_err_validation_failed;
    }
    var length: usize = 0;
    const err = internal_decode(source, span, null, 0, false, &length);
    out_length.* = length;
    return err;
}

/// Compare a bounded span with an exact ASCII literal.
export fn xml_span_equal(
    source: [*c]const u8,
    source_len: usize,
    span: xml_span_t,
    literal: [*c]const u8,
) callconv(.c) bool {
    if ((source == null) or (literal == null) or !priv_xml_span_valid(source_len, span)) {
        return false;
    }
    const length = c_strlen(literal);
    return (@as(usize, span.length) == length) and
        priv_xml_bytes_equal(source, @as(usize, span.offset), literal, length);
}

/// Compare the namespace-local tail of a bounded span with an ASCII literal:
/// the bytes after the optional colon.
export fn xml_span_local_equal(
    source: [*c]const u8,
    source_len: usize,
    span: xml_span_t,
    literal: [*c]const u8,
) callconv(.c) bool {
    if ((source == null) or !priv_xml_span_valid(source_len, span)) {
        return false;
    }
    var offset: c_uint = span.offset;
    var length: c_uint = span.length;
    var i: c_uint = 0;
    while (i < span.length) : (i += 1) {
        if (source[span.offset + i] == ':') {
            offset = span.offset + i + 1;
            length = span.length - i - 1;
        }
    }
    return xml_span_equal(source, source_len, .{ .offset = offset, .length = length }, literal);
}

/// Compare two entity-decoded spans from the same immutable source.
export fn xml_decoded_equal(
    source: [*c]const u8,
    source_len: usize,
    left: xml_span_t,
    right: xml_span_t,
) callconv(.c) bool {
    if (source == null) {
        return false;
    }
    var left_size: usize = 0;
    var right_size: usize = 0;
    if ((xml_decoded_size(source, source_len, left, &left_size) != k_ra8_ok) or
        (xml_decoded_size(source, source_len, right, &right_size) != k_ra8_ok) or
        (left_size != right_size))
    {
        return false;
    }
    var li = PrivDecodeCursor{ .position = left.offset, .end = @as(usize, left.offset) + left.length };
    var ri = PrivDecodeCursor{ .position = right.offset, .end = @as(usize, right.offset) + right.length };
    var i: usize = 0;
    while (i < left_size) : (i += 1) {
        var lb: u8 = 0;
        var rb: u8 = 0;
        // mcdc-deactivated: xml_decoded_equal byte-walk status guards;
        // xml_decoded_size already decoded both spans end to end and reported
        // exactly left_size/right_size bytes, so for every i < left_size both
        // internal_decoded_byte() calls are re-walking bytes proven decodable --
        // only the byte-inequality condition can flip.
        if ((internal_decoded_byte(source, &li, &lb) != k_ra8_ok) or
            (internal_decoded_byte(source, &ri, &rb) != k_ra8_ok) or (lb != rb))
        {
            return false;
        }
    }
    return true;
}

/// Validate canonical UTF-8 XML 1.0 characters over a byte range: rejects
/// overlong encoding, invalid continuations, controls, and surrogates.
export fn priv_xml_raw(source: [*c]const u8, start: usize, end: usize) callconv(.c) ra8_err_t {
    var cursor = start;
    while (cursor < end) {
        var cp: u32 = 0;
        var used: usize = 0;
        const err = internal_utf8_next(source, end, cursor, &cp, &used);
        if (err != k_ra8_ok) {
            return err;
        }
        cursor += used;
    }
    return k_ra8_ok;
}

// ---------------------------------------------------------------------------
// Zig-side unit tests. The library is pure, so these run with no seams.
// ---------------------------------------------------------------------------
const testing = std.testing;

fn decodedEqual(expect: []const u8, got: [*c]const u8, len: usize) bool {
    if (got == null) return false;
    var i: usize = 0;
    while (i < len) : (i += 1) {
        if (got[i] != expect[i]) return false;
    }
    return (len < expect.len) or (expect[len] == 0) or true;
}

test "utf8 scalar validation accepts canonical runs and rejects the rest" {
    const ok_src = "a\t\n\r" ++ "\xC3\xA9" ++ "\xE2\x82\xAC" ++ "\xF0\x9F\x92\xA9";
    try testing.expectEqual(k_ra8_ok, priv_xml_raw(ok_src.ptr, 0, ok_src.len));
    const overlong = "\xC0\x80";
    try testing.expectEqual(k_ra8_err_validation_failed, priv_xml_raw(overlong.ptr, 0, overlong.len));
    const bare_continuation = "\x80";
    try testing.expectEqual(k_ra8_err_validation_failed, priv_xml_raw(bare_continuation.ptr, 0, 1));
    const surrogate = "\xED\xA0\x80";
    try testing.expectEqual(k_ra8_err_validation_failed, priv_xml_raw(surrogate.ptr, 0, 3));
    const beyond_max = "\xF4\x90\x80\x80";
    try testing.expectEqual(k_ra8_err_validation_failed, priv_xml_raw(beyond_max.ptr, 0, 4));
    const nul = "\x00";
    try testing.expectEqual(k_ra8_err_validation_failed, priv_xml_raw(nul.ptr, 0, 1));
    const truncated = "\xC3";
    try testing.expectEqual(k_ra8_err_validation_failed, priv_xml_raw(truncated.ptr, 0, 1));
}

test "predefined entities decode" {
    const src = "&amp;&lt;&gt;&quot;&apos;";
    var out: [8]u8 = undefined;
    var len: usize = 0;
    const span = xml_span_t{ .offset = 0, .length = src.len };
    try testing.expectEqual(k_ra8_ok, xml_decode(src.ptr, src.len, span, &out, out.len, &len));
    try testing.expectEqual(@as(usize, 5), len);
    try testing.expectEqualStrings("&<>\"'", out[0..len]);
}

test "numeric references: decimal, hex, and the rejection set" {
    var len: usize = 0;
    const dec = "&#38;";
    try testing.expectEqual(k_ra8_ok, xml_decoded_size(dec.ptr, dec.len, .{ .offset = 0, .length = dec.len }, &len));
    try testing.expectEqual(@as(usize, 1), len);
    const hex = "&#x26;";
    try testing.expectEqual(k_ra8_ok, xml_decoded_size(hex.ptr, hex.len, .{ .offset = 0, .length = hex.len }, &len));
    try testing.expectEqual(@as(usize, 1), len);
    const hex_upper_x = "&#X41;";
    try testing.expectEqual(k_ra8_ok, xml_decoded_size(hex_upper_x.ptr, hex_upper_x.len, .{ .offset = 0, .length = hex_upper_x.len }, &len));
    try testing.expectEqual(@as(usize, 1), len);
    const bad_digit = "&#3z;";
    try testing.expectEqual(k_ra8_err_validation_failed, xml_decoded_size(bad_digit.ptr, bad_digit.len, .{ .offset = 0, .length = bad_digit.len }, &len));
    const overflow = "&#99999999999999999999;";
    try testing.expectEqual(k_ra8_err_validation_failed, xml_decoded_size(overflow.ptr, overflow.len, .{ .offset = 0, .length = overflow.len }, &len));
    const empty = "&#;";
    try testing.expectEqual(k_ra8_err_validation_failed, xml_decoded_size(empty.ptr, empty.len, .{ .offset = 0, .length = empty.len }, &len));
    const unterminated = "&#65";
    try testing.expectEqual(k_ra8_err_validation_failed, xml_decoded_size(unterminated.ptr, unterminated.len, .{ .offset = 0, .length = unterminated.len }, &len));
    const control = "&#0;";
    try testing.expectEqual(k_ra8_err_validation_failed, xml_decoded_size(control.ptr, control.len, .{ .offset = 0, .length = control.len }, &len));
    const unknown_name = "&nbsp;";
    try testing.expectEqual(k_ra8_err_validation_failed, xml_decoded_size(unknown_name.ptr, unknown_name.len, .{ .offset = 0, .length = unknown_name.len }, &len));
    const short = "&a";
    try testing.expectEqual(k_ra8_err_validation_failed, xml_decoded_size(short.ptr, short.len, .{ .offset = 0, .length = short.len }, &len));
}

test "decode round-trips mixed text and entities" {
    const src = "a&amp;b&#x42;c";
    var out: [8]u8 = undefined;
    var len: usize = 0;
    try testing.expectEqual(k_ra8_ok, xml_decode(src.ptr, src.len, .{ .offset = 0, .length = src.len }, &out, out.len, &len));
    try testing.expectEqual(@as(usize, 5), len);
    try testing.expectEqualStrings("a&bBc", out[0..len]);
}

test "decode rejects an undersized destination; prefix clips at entity boundaries" {
    const src = "a&amp;b";
    var out: [8]u8 = undefined;
    var len: usize = 0;
    try testing.expectEqual(k_ra8_err_no_mem, xml_decode(src.ptr, src.len, .{ .offset = 0, .length = src.len }, &out, 3, &len));
    // Decoded size is 3; capacity must hold the NUL, so 3 is one short.
    try testing.expectEqual(k_ra8_ok, xml_decode(src.ptr, src.len, .{ .offset = 0, .length = src.len }, &out, 4, &len));
    try testing.expectEqual(@as(usize, 3), len);
    try testing.expectEqualStrings("a&b", out[0..len]);

    var pfx: [4]u8 = undefined;
    var plen: usize = 0;
    const wide = "a&amp;b&amp;c";
    try testing.expectEqual(k_ra8_ok, xml_decode_prefix(wide.ptr, wide.len, .{ .offset = 0, .length = wide.len }, &pfx, pfx.len, &plen));
    try testing.expectEqual(@as(usize, 3), plen);
    try testing.expectEqualStrings("a&b", pfx[0..plen]);
}

test "span validity and literal comparisons" {
    const src = "svg:rect";
    try testing.expect(priv_xml_span_valid(src.len, .{ .offset = 0, .length = src.len }));
    try testing.expect(!priv_xml_span_valid(src.len, .{ .offset = 9, .length = 0 }));
    try testing.expect(!priv_xml_span_valid(src.len, .{ .offset = 0, .length = src.len + 1 }));
    try testing.expect(xml_span_equal(src.ptr, src.len, .{ .offset = 4, .length = 4 }, "rect"));
    try testing.expect(!xml_span_equal(src.ptr, src.len, .{ .offset = 4, .length = 4 }, "rectx"));
    try testing.expect(xml_span_local_equal(src.ptr, src.len, .{ .offset = 0, .length = 8 }, "rect"));
    try testing.expect(xml_span_local_equal(src.ptr, src.len, .{ .offset = 4, .length = 4 }, "rect"));
}

test "decoded equality compares through entities" {
    const src = "&#65;A&#66;B";
    try testing.expect(xml_decoded_equal(src.ptr, src.len, .{ .offset = 0, .length = 5 }, .{ .offset = 5, .length = 1 }));
    try testing.expect(!xml_decoded_equal(src.ptr, src.len, .{ .offset = 5, .length = 1 }, .{ .offset = 6, .length = 1 }));
    try testing.expect(!xml_decoded_equal(null, src.len, .{ .offset = 0, .length = 1 }, .{ .offset = 5, .length = 1 }));
}

test "null pointers take the null_ptr tail" {
    var len: usize = 0;
    const src = "a";
    try testing.expectEqual(k_ra8_err_null_ptr, xml_decode(null, 1, .{ .offset = 0, .length = 1 }, null, 1, &len));
    try testing.expectEqual(k_ra8_err_null_ptr, xml_decode(src.ptr, 1, .{ .offset = 0, .length = 1 }, null, 1, &len));
    try testing.expectEqual(k_ra8_err_null_ptr, xml_decoded_size(src.ptr, 1, .{ .offset = 0, .length = 1 }, null));
}

test "raw validation walks the whole range, not a prefix" {
    const src = "ok\xFF";
    try testing.expectEqual(k_ra8_err_validation_failed, priv_xml_raw(src.ptr, 0, src.len));
    // start inside the range still validates to the end
    try testing.expectEqual(k_ra8_err_validation_failed, priv_xml_raw(src.ptr, 2, 3));
}
