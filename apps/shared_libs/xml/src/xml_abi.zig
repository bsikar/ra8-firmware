//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Zig implementation of the xml decode half (RA8FW-1157): the C-era
//! src/xml_decode.c ported verbatim behind the unchanged C headers in inc/.
//! The decode half is pure: canonical UTF-8 scalar validation and encoding,
//! the five predefined entities plus decimal and hex character references,
//! bounded span decoding into caller-owned storage, and the decoded
//! comparisons. No reader state is touched here; the markup-scanning half
//! (RA8FW-1158) and the DOCTYPE validator (RA8FW-1206) share this file, so
//! the whole `xml` reader library is Zig.

const std = @import("std");

// ---- ra8_err_t (libs/ra8_core/inc/ra8_err.h, enum : uint32_t) --------------
const ra8_err_t = u16;
const k_ra8_ok: ra8_err_t = 0;
const k_ra8_err_no_mem: ra8_err_t = 0x102;
const k_ra8_err_null_ptr: ra8_err_t = 0x504;
const k_ra8_err_invalid_size: ra8_err_t = 0x105;
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

const k_priv_xml_encoding_bytes: usize = 5;
const k_priv_xml_cdata_open_bytes: usize = 9;
const k_priv_xml_doctype_open_bytes: usize = 9;
const k_priv_utf8_bom_first: u8 = 0xEF;
const k_priv_utf8_bom_second: u8 = 0xBB;
const k_priv_utf8_bom_third: u8 = 0xBF;

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
// Markup-scanning half (RA8FW-1158): the C-era src/xml.c ported verbatim.
// QName scanning, attribute parsing and duplicate rejection, start/end/text/
// CDATA/comment/PI/declaration events, and the pull-reader driver.
// ---------------------------------------------------------------------------

// ---- xml.h reader types (extern layout matches the C header) ---------------
const k_xml_max_element_depth: c_ushort = 499;
const k_xml_workspace_frames: c_ushort = 512;

const k_xml_event_none: u8 = 0;
const k_xml_event_start: u8 = 1;
const k_xml_event_end: u8 = 2;
const k_xml_event_text: u8 = 3;
const k_xml_event_cdata: u8 = 4;

const xml_attribute_t = extern struct { name: xml_span_t, value: xml_span_t };
const xml_frame_t = extern struct { name_offset: c_uint, name_length: c_ushort, consumer: c_ushort };
const xml_workspace_t = extern struct { frames: [k_xml_workspace_frames]xml_frame_t };
const xml_event_t = extern struct {
    markup: xml_span_t,
    name: xml_span_t,
    depth: c_ushort,
    attribute_count: c_ushort,
    kind: u8,
    self_closing: u8,
};
const xml_attr_cursor_t = extern struct { position: c_uint, emitted: c_ushort };
const xml_reader_t = extern struct {
    source: [*c]const u8,
    source_len: usize,
    position: usize,
    workspace: [*c]xml_workspace_t,
    stack_size: c_ushort,
    root_count: u8,
    root_closed: u8,
    declaration_seen: u8,
    doctype_seen: u8,
    finished: u8,
};

// ---------------------------------------------------------------------------
// DOCTYPE validator (RA8FW-1206): the C-era src/xml_doctype.c ported
// verbatim. Accepts and ignores lexically valid bare, SYSTEM, and PUBLIC
// declarations before the root without resolving external resources;
// internal subsets and unsupported declaration forms fail closed.
// ---------------------------------------------------------------------------

/// Skip XML spacing and report whether at least one byte was consumed;
/// advances only inside the supplied half-open byte range.
fn internal_skip_space(source: [*c]const u8, end: usize, position: *usize) bool {
    const initial = position.*;
    while ((position.* < end) and internal_space(source[position.*])) {
        position.* += 1;
    }
    return position.* != initial;
}

/// The XML PubidChar subset accepted in public identifiers: XML spacing,
/// alphanumerics, and XML punctuation.
fn internal_pubid_byte(byte: u8) bool {
    const punctuation = "-'()+,./:=?;!*#@$_%";
    return (byte == ' ') or (byte == '\r') or (byte == '\n') or
        ((byte >= 'A') and (byte <= 'Z')) or
        ((byte >= 'a') and (byte <= 'z')) or
        ((byte >= '0') and (byte <= '9')) or
        ((byte != 0) and (std.mem.indexOfScalar(u8, punctuation, byte) != null));
}

/// Parse one quoted system or public identifier literal: respects either
/// quote and validates optional PubidChar bytes; no resource is fetched.
fn internal_literal(source: [*c]const u8, end: usize, position: *usize, public_id: bool) ra8_err_t {
    if ((position.* >= end) or
        ((source[position.*] != '\'') and (source[position.*] != '"')))
    {
        return k_ra8_err_validation_failed;
    }
    const quote = source[position.*];
    position.* += 1;
    const start = position.*;
    while ((position.* < end) and (source[position.*] != quote)) {
        if (public_id and !internal_pubid_byte(source[position.*])) {
            return k_ra8_err_validation_failed;
        }
        position.* += 1;
    }
    if (position.* >= end) {
        return k_ra8_err_validation_failed;
    }
    const err: ra8_err_t = if (public_id) k_ra8_ok else priv_xml_raw(source, start, position.*);
    position.* += 1;
    return err;
}

/// Consume an exact ASCII keyword followed by required spacing; matching is
/// intentionally case-sensitive.
fn internal_keyword(source: [*c]const u8, end: usize, position: *usize, keyword: []const u8) ra8_err_t {
    if ((position.* + keyword.len > end) or
        !priv_xml_bytes_equal(source, position.*, keyword.ptr, keyword.len))
    {
        return k_ra8_err_validation_failed;
    }
    position.* += keyword.len;
    return if (internal_skip_space(source, end, position)) k_ra8_ok else k_ra8_err_validation_failed;
}

/// Parse a SYSTEM or PUBLIC external identifier; external identifiers are
/// checked but never resolved.
fn internal_external_id(source: [*c]const u8, end: usize, position: *usize) ra8_err_t {
    if (((position.* + 6) <= end) and priv_xml_bytes_equal(source, position.*, "SYSTEM".ptr, "SYSTEM".len)) {
        const err = internal_keyword(source, end, position, "SYSTEM");
        return if (err == k_ra8_ok) internal_literal(source, end, position, false) else err;
    }
    if (((position.* + 6) <= end) and priv_xml_bytes_equal(source, position.*, "PUBLIC".ptr, "PUBLIC".len)) {
        var err = internal_keyword(source, end, position, "PUBLIC");
        if (err == k_ra8_ok) {
            err = internal_literal(source, end, position, true);
        }
        if ((err == k_ra8_ok) and !internal_skip_space(source, end, position)) {
            err = k_ra8_err_validation_failed;
        }
        return if (err == k_ra8_ok) internal_literal(source, end, position, false) else err;
    }
    return k_ra8_err_validation_failed;
}

/// Bounded external-only DOCTYPE validator: the markup scanner reaches it at
/// the same seam the C-era TU used, and the export keeps the symbol stable
/// for the C callers that still link the reader archive.
export fn priv_xml_doctype(reader: [*c]xml_reader_t) callconv(.c) ra8_err_t {
    const end = reader.*.source_len;
    var position = reader.*.position;
    if ((reader.*.root_count != 0) or (reader.*.doctype_seen != 0) or
        ((position + k_priv_xml_doctype_open_bytes) > end) or
        !internal_matches(reader.*.source, position, "<!DOCTYPE"))
    {
        return k_ra8_err_validation_failed;
    }
    position += k_priv_xml_doctype_open_bytes;
    if (!internal_skip_space(reader.*.source, end, &position)) {
        return k_ra8_err_validation_failed;
    }
    var name_end: usize = 0;
    if (priv_xml_qname(reader.*.source, end, position, &name_end) != k_ra8_ok) {
        return k_ra8_err_validation_failed;
    }
    position = name_end;
    const separated = internal_skip_space(reader.*.source, end, &position);
    if ((position < end) and (reader.*.source[position] != '>')) {
        if (!separated or (internal_external_id(reader.*.source, end, &position) != k_ra8_ok)) {
            return k_ra8_err_validation_failed;
        }
        _ = internal_skip_space(reader.*.source, end, &position);
    }
    if ((position >= end) or (reader.*.source[position] != '>')) {
        return k_ra8_err_validation_failed;
    }
    reader.*.doctype_seen = 1;
    reader.*.position = position + 1;
    return k_ra8_ok;
}

fn empty_attribute() xml_attribute_t {
    return .{ .name = .{ .offset = 0, .length = 0 }, .value = .{ .offset = 0, .length = 0 } };
}

fn empty_event() xml_event_t {
    return .{
        .markup = .{ .offset = 0, .length = 0 },
        .name = .{ .offset = 0, .length = 0 },
        .depth = 0,
        .attribute_count = 0,
        .kind = k_xml_event_none,
        .self_closing = 0,
    };
}

/// XML spacing: space, tab, carriage return, and line feed only.
fn internal_space(c: u8) bool {
    return (c == ' ') or (c == '\t') or (c == '\n') or (c == '\r');
}

/// Locale-independent ASCII-letter classification.
fn internal_ascii_letter(c: u8) bool {
    return ((c >= 'A') and (c <= 'Z')) or ((c >= 'a') and (c <= 'z'));
}

/// Supported NCName start byte: ASCII letter or underscore.
fn internal_name_start(c: u8) bool {
    return internal_ascii_letter(c) or (c == '_');
}

/// Supported later NCName byte: start set plus digits, dot, and dash.
fn internal_name_continue(c: u8) bool {
    return internal_name_start(c) or ((c >= '0') and (c <= '9')) or (c == '.') or (c == '-');
}

/// Compare @p length source bytes at two offsets (the C memcmp name checks).
fn internal_bytes_same(source: [*c]const u8, a: c_uint, b: c_uint, length: c_uint) bool {
    var i: c_uint = 0;
    while (i < length) : (i += 1) {
        if (source[a + i] != source[b + i]) {
            return false;
        }
    }
    return true;
}

/// Compare bounded source bytes against a literal of known length.
fn internal_matches(source: [*c]const u8, offset: usize, literal: []const u8) bool {
    return priv_xml_bytes_equal(source, offset, literal.ptr, literal.len);
}

/// Scan the supported QName subset over a bounded byte range: one or two
/// ASCII NCName components separated by at most one interior colon.
export fn priv_xml_qname(source: [*c]const u8, end: usize, start: usize, out_end: [*c]usize) callconv(.c) ra8_err_t {
    if ((start >= end) or !internal_name_start(source[start])) {
        return k_ra8_err_validation_failed;
    }
    var cursor: usize = start + 1;
    var saw_colon = false;
    while (cursor < end) {
        const byte = source[cursor];
        if (internal_name_continue(byte)) {
            cursor += 1;
        } else if ((byte == ':') and !saw_colon and ((cursor + 1) < end) and
            internal_name_start(source[cursor + 1]))
        {
            saw_colon = true;
            cursor += 2;
        } else {
            break;
        }
    }
    out_end.* = cursor;
    return k_ra8_ok;
}

/// Initialise source-order attribute iteration for a start event.
export fn xml_attr_begin(event: [*c]const xml_event_t, cursor: [*c]xml_attr_cursor_t) callconv(.c) void {
    if ((event != null) and (cursor != null)) {
        cursor.*.position = event.*.name.offset +% event.*.name.length;
        cursor.*.emitted = 0;
    }
}

/// Parse the equals sign, quote, and value of one XML attribute. Skips
/// allowed XML whitespace, requires a quoted value, rejects a literal
/// less-than sign, and advances the caller past the closing quote.
fn internal_attr_value(source: [*c]const u8, end: u32, position: *u32, out: *xml_attribute_t) ra8_err_t {
    var cursor: u32 = position.*;
    while ((cursor < end) and internal_space(source[cursor])) {
        cursor += 1;
    }
    if ((cursor >= end) or (source[cursor] != '=')) {
        return k_ra8_err_validation_failed;
    }
    cursor += 1;
    while ((cursor < end) and internal_space(source[cursor])) {
        cursor += 1;
    }
    if ((cursor >= end) or ((source[cursor] != '\'') and (source[cursor] != '"'))) {
        return k_ra8_err_validation_failed;
    }
    const quote = source[cursor];
    cursor += 1;
    const value_start: u32 = cursor;
    while ((cursor < end) and (source[cursor] != quote)) {
        if (source[cursor] == '<') {
            return k_ra8_err_validation_failed;
        }
        cursor += 1;
    }
    if (cursor >= end) {
        return k_ra8_err_validation_failed;
    }
    out.*.value = .{ .offset = value_start, .length = cursor - value_start };
    position.* = cursor + 1;
    return k_ra8_ok;
}

/// Parse one attribute at a bounded tag cursor: QName, quoting, decoded
/// value, and cursor progression.
fn internal_attr_parse(source: [*c]const u8, source_len: usize, end: u32, cursor: [*c]xml_attr_cursor_t, out: *xml_attribute_t) ra8_err_t {
    var position: u32 = cursor.*.position;
    while ((position < end) and internal_space(source[position])) {
        position += 1;
    }
    const name_start: u32 = position;
    var name_end: usize = 0;
    if (priv_xml_qname(source, @as(usize, end), @as(usize, name_start), &name_end) != k_ra8_ok) {
        return k_ra8_err_validation_failed;
    }
    position = @intCast(name_end);
    out.*.name = .{ .offset = name_start, .length = position - name_start };
    var err = internal_attr_value(source, end, &position, out);
    if (err == k_ra8_ok) {
        cursor.*.position = position;
        cursor.*.emitted += 1;
        var decoded: usize = 0;
        err = xml_decoded_size(source, source_len, out.*.value, &decoded);
    }
    return err;
}

/// Detect a duplicate attribute name by replaying prior source-order
/// attributes and comparing raw QName bytes. Fails closed on replay
/// inconsistency.
fn internal_attr_duplicate(source: [*c]const u8, source_len: usize, event: [*c]const xml_event_t, current: [*c]const xml_attribute_t, prior_count: c_ushort) bool {
    var prior: xml_attr_cursor_t = .{ .position = 0, .emitted = 0 };
    xml_attr_begin(event, &prior);
    const end: u32 = event.*.markup.offset +% event.*.markup.length -% 1;
    var i: c_ushort = 0;
    while (i < prior_count) : (i += 1) {
        var attribute = empty_attribute();
        if (internal_attr_parse(source, source_len, end, &prior, &attribute) != k_ra8_ok) {
            return true;
        }
        if ((attribute.name.length == current.*.name.length) and
            internal_bytes_same(source, attribute.name.offset, current.*.name.offset, current.*.name.length))
        {
            return true;
        }
    }
    return false;
}

/// Return the next source-order attribute of a start event.
export fn xml_attr_next(source: [*c]const u8, source_len: usize, event: [*c]const xml_event_t, cursor: [*c]xml_attr_cursor_t, out_attribute: [*c]xml_attribute_t, out_has_value: [*c]bool) callconv(.c) ra8_err_t {
    if ((source == null) or (event == null) or (cursor == null) or
        (out_attribute == null) or (out_has_value == null))
    {
        return k_ra8_err_null_ptr;
    }
    if (cursor.*.emitted >= event.*.attribute_count) {
        out_has_value.* = false;
        return k_ra8_ok;
    }
    if (!priv_xml_span_valid(source_len, event.*.markup) or
        !priv_xml_span_valid(source_len, event.*.name) or (event.*.markup.length == 0))
    {
        return k_ra8_err_validation_failed;
    }
    const end: u32 = event.*.markup.offset +% event.*.markup.length -% 1;
    const err = internal_attr_parse(source, source_len, end, cursor, out_attribute);
    out_has_value.* = (err == k_ra8_ok);
    return err;
}

/// Locate a markup closing byte while respecting quotes: ignores
/// greater-than bytes inside single or double quoted values.
fn internal_markup_end(source: [*c]const u8, length: usize, start: usize, out_end: *usize) ra8_err_t {
    var quote: u8 = 0;
    var i: usize = start + 1;
    while (i < length) : (i += 1) {
        const byte = source[i];
        if (quote != 0) {
            if (byte == quote) {
                quote = 0;
            }
            continue;
        }
        if ((byte == '\'') or (byte == '"')) {
            quote = byte;
            continue;
        }
        if (byte == '>') {
            out_end.* = i;
            return k_ra8_ok;
        }
    }
    return k_ra8_err_validation_failed;
}

/// Count and validate one start tag's attributes: unique names, quoted
/// decoded values, and an exact tag tail.
fn internal_attributes(source: [*c]const u8, source_len: usize, event: [*c]xml_event_t) ra8_err_t {
    var cursor: xml_attr_cursor_t = .{ .position = 0, .emitted = 0 };
    xml_attr_begin(event, &cursor);
    const end: u32 = event.*.markup.offset +% event.*.markup.length -% 1;
    var count: c_ushort = 0;
    while (true) {
        var position: u32 = cursor.position;
        while ((position < end) and internal_space(source[position])) {
            position += 1;
        }
        if ((position >= end) or (source[position] == '/')) {
            cursor.position = position;
            break;
        }
        var attribute = empty_attribute();
        const err = internal_attr_parse(source, source_len, end, &cursor, &attribute);
        // mcdc-deactivated: internal_attributes attribute-count saturation
        // guard; it exists to stop `count` wrapping the uint16_t
        // event->attribute_count, and flipping it needs 65535 mutually
        // distinct attributes on ONE element while internal_attr_duplicate
        // re-parses every prior attribute per new one -- a quadratic
        // ~2.1e9-parse input that no bounded unit test can drive.
        if ((err != k_ra8_ok) or (count == @as(c_ushort, 65535)) or
            internal_attr_duplicate(source, source_len, event, &attribute, count))
        {
            return k_ra8_err_validation_failed;
        }
        count += 1;
    }
    if (event.*.self_closing != 0) {
        // mcdc-deactivated: internal_attributes self-closing slash recheck;
        // the attribute loop above breaks only on `position >= end` or
        // `source[position] == '/'`, and self_closing was set from a
        // non-space '/' that lies inside the still-unscanned range, so the
        // loop necessarily stopped on that slash -- both conditions are
        // constant-false here.
        if ((cursor.position >= end) or (source[cursor.position] != '/')) {
            return k_ra8_err_validation_failed;
        }
        cursor.position += 1;
    }
    while ((cursor.position < end) and internal_space(source[cursor.position])) {
        cursor.position += 1;
    }
    if (cursor.position != end) {
        return k_ra8_err_validation_failed;
    }
    event.*.attribute_count = count;
    return k_ra8_ok;
}

/// Parse one start element and update nesting. Validates QName,
/// attributes, root, and depth before pushing a live frame.
fn internal_start(reader: [*c]xml_reader_t, end: usize, event: [*c]xml_event_t) ra8_err_t {
    var cursor: usize = reader.*.position + 1;
    const name_start: usize = cursor;
    if ((priv_xml_qname(reader.*.source, end, name_start, &cursor) != k_ra8_ok) or
        (reader.*.root_closed != 0) or
        (@as(usize, reader.*.stack_size) >= @as(usize, k_xml_max_element_depth)))
    {
        return k_ra8_err_validation_failed;
    }
    event.*.kind = k_xml_event_start;
    event.*.depth = reader.*.stack_size;
    event.*.name = .{ .offset = @intCast(name_start), .length = @intCast(cursor - name_start) };
    event.*.markup = .{ .offset = @intCast(reader.*.position), .length = @intCast(end - reader.*.position + 1) };
    var tail: usize = end;
    while ((tail > cursor) and internal_space(reader.*.source[tail - 1])) {
        tail -= 1;
    }
    event.*.self_closing = if ((tail > cursor) and (reader.*.source[tail - 1] == '/')) 1 else 0;
    const attr_err = internal_attributes(reader.*.source, reader.*.source_len, event);
    if (attr_err != k_ra8_ok) {
        return attr_err;
    }
    if (reader.*.stack_size == 0) {
        if (reader.*.root_count != 0) {
            return k_ra8_err_validation_failed;
        }
        reader.*.root_count = 1;
    }
    if (event.*.self_closing != 0) {
        if (reader.*.stack_size == 0) {
            reader.*.root_closed = 1;
        }
    } else {
        const frame = &reader.*.workspace.*.frames[reader.*.stack_size];
        reader.*.stack_size += 1;
        frame.* = .{
            .name_offset = event.*.name.offset,
            .name_length = @truncate(event.*.name.length),
            .consumer = 0,
        };
    }
    reader.*.position = end + 1;
    return k_ra8_ok;
}

/// Parse one close element and update nesting. Allows trailing XML spacing
/// and requires an exact open-name match.
fn internal_end(reader: [*c]xml_reader_t, end: usize, event: [*c]xml_event_t) ra8_err_t {
    var cursor: usize = reader.*.position + 2;
    const name_start: usize = cursor;
    if (priv_xml_qname(reader.*.source, end, name_start, &cursor) != k_ra8_ok) {
        return k_ra8_err_validation_failed;
    }
    const name_end: usize = cursor;
    while ((cursor < end) and internal_space(reader.*.source[cursor])) {
        cursor += 1;
    }
    if ((cursor != end) or (reader.*.stack_size == 0)) {
        return k_ra8_err_validation_failed;
    }
    const frame = &reader.*.workspace.*.frames[reader.*.stack_size - 1];
    const name_length: usize = name_end - name_start;
    if ((name_length != @as(usize, frame.*.name_length)) or
        !internal_bytes_same(reader.*.source, @intCast(name_start), frame.*.name_offset, @intCast(name_length)))
    {
        return k_ra8_err_validation_failed;
    }
    reader.*.stack_size -= 1;
    event.*.kind = k_xml_event_end;
    event.*.depth = reader.*.stack_size;
    event.*.name = .{ .offset = @intCast(name_start), .length = @intCast(name_length) };
    event.*.markup = .{ .offset = @intCast(reader.*.position), .length = @intCast(end - reader.*.position + 1) };
    if (reader.*.stack_size == 0) {
        reader.*.root_closed = 1;
    }
    reader.*.position = end + 1;
    return k_ra8_ok;
}

/// Locate a literal terminator in a bounded source: forward byte-exact
/// search without sentinel reads.
fn internal_terminator(source: [*c]const u8, length: usize, start: usize, terminator: []const u8, out_start: *usize) ra8_err_t {
    const term_len = terminator.len;
    var i: usize = start;
    while ((i + term_len) <= length) : (i += 1) {
        if (internal_matches(source, i, terminator)) {
            out_start.* = i;
            return k_ra8_ok;
        }
    }
    return k_ra8_err_validation_failed;
}

/// Validate and skip one XML comment: rejects an internal double dash and a
/// payload ending in dash.
fn internal_comment(reader: [*c]xml_reader_t) ra8_err_t {
    const content: usize = reader.*.position + 4;
    var term: usize = 0;
    var err = internal_terminator(reader.*.source, reader.*.source_len, content, "-->", &term);
    if (err != k_ra8_ok) {
        return err;
    }
    if ((term > content) and (reader.*.source[term - 1] == '-')) {
        return k_ra8_err_validation_failed;
    }
    var i: usize = content;
    while ((i + 1) < term) : (i += 1) {
        if ((reader.*.source[i] == '-') and (reader.*.source[i + 1] == '-')) {
            return k_ra8_err_validation_failed;
        }
    }
    err = priv_xml_raw(reader.*.source, content, term);
    if (err == k_ra8_ok) {
        reader.*.position = term + 3;
    }
    return err;
}

/// Test for the reserved case-insensitive PI target XML.
fn internal_xml_target(source: [*c]const u8, start: usize, end: usize) bool {
    return ((end - start) == 3) and
        ((source[start] | 0x20) == 'x') and
        ((source[start + 1] | 0x20) == 'm') and
        ((source[start + 2] | 0x20) == 'l');
}

/// Validate an XML declaration encoding name: exactly `UTF-8` or `utf-8`.
fn internal_encoding(source: [*c]const u8, value: xml_span_t) bool {
    return (@as(usize, value.length) == k_priv_xml_encoding_bytes) and
        (internal_matches(source, @as(usize, value.offset), "UTF-8") or
            internal_matches(source, @as(usize, value.offset), "utf-8"));
}

/// Validate one ordered XML-declaration pseudo-attribute: version first,
/// then optional encoding and standalone.
fn internal_declaration_attr(source: [*c]const u8, source_len: usize, attribute: [*c]const xml_attribute_t, ordinal: c_ushort, saw_encoding: *bool, saw_standalone: *bool) bool {
    if (ordinal == 0) {
        return xml_span_equal(source, source_len, attribute.*.name, "version") and
            xml_span_equal(source, source_len, attribute.*.value, "1.0");
    }
    if (!saw_encoding.* and !saw_standalone.* and
        xml_span_equal(source, source_len, attribute.*.name, "encoding"))
    {
        saw_encoding.* = true;
        return internal_encoding(source, attribute.*.value);
    }
    if (!saw_standalone.* and
        xml_span_equal(source, source_len, attribute.*.name, "standalone"))
    {
        saw_standalone.* = true;
        return xml_span_equal(source, source_len, attribute.*.value, "yes") or
            xml_span_equal(source, source_len, attribute.*.value, "no");
    }
    return false;
}

/// Validate and skip the leading XML declaration: byte-zero/BOM-zero
/// placement and ordered declaration grammar.
fn internal_declaration(reader: [*c]xml_reader_t, target_end: usize, term: usize) ra8_err_t {
    const initial = (reader.*.position == 0) or
        ((reader.*.position == 3) and (reader.*.source[0] == k_priv_utf8_bom_first));
    // mcdc-deactivated: internal_declaration placement gate; `initial` is
    // only true at offset 0 or at offset 3 behind a BOM, and both
    // declaration_seen and root_count are set by passes that leave
    // reader->position beyond those offsets, so conditions 2 and 3 cannot be
    // true while condition 1 is false. Condition 5 is likewise constant:
    // internal_pi has already rejected `(target_end < term) &&
    // !internal_space(source[target_end])`, so reaching here with
    // `target_end < term` implies the byte IS spacing. Only the
    // `target_end >= term` condition varies.
    if (!initial or (reader.*.declaration_seen != 0) or (reader.*.root_count != 0) or
        (target_end >= term) or !internal_space(reader.*.source[target_end]))
    {
        return k_ra8_err_validation_failed;
    }
    var cursor: xml_attr_cursor_t = .{ .position = @intCast(target_end), .emitted = 0 };
    var saw_encoding = false;
    var saw_standalone = false;
    var count: c_ushort = 0;
    while (@as(usize, cursor.position) < term) {
        while ((@as(usize, cursor.position) < term) and
            internal_space(reader.*.source[cursor.position]))
        {
            cursor.position += 1;
        }
        if (@as(usize, cursor.position) == term) {
            break;
        }
        var attribute = empty_attribute();
        const err = internal_attr_parse(reader.*.source, reader.*.source_len, @intCast(term), &cursor, &attribute);
        if ((err != k_ra8_ok) or
            !internal_declaration_attr(reader.*.source, reader.*.source_len, &attribute, count, &saw_encoding, &saw_standalone))
        {
            return k_ra8_err_validation_failed;
        }
        count += 1;
    }
    if (count == 0) {
        return k_ra8_err_validation_failed;
    }
    reader.*.declaration_seen = 1;
    reader.*.position = term + 2;
    return k_ra8_ok;
}

/// Validate and skip a PI or the leading XML declaration. QName-validates
/// the target and reserves case-insensitive XML.
fn internal_pi(reader: [*c]xml_reader_t) ra8_err_t {
    const target_start: usize = reader.*.position + 2;
    var term: usize = 0;
    var err = internal_terminator(reader.*.source, reader.*.source_len, target_start, "?>", &term);
    if (err != k_ra8_ok) {
        return err;
    }
    var target_end: usize = 0;
    err = priv_xml_qname(reader.*.source, term, target_start, &target_end);
    if ((err != k_ra8_ok) or
        ((target_end < term) and !internal_space(reader.*.source[target_end])))
    {
        return k_ra8_err_validation_failed;
    }
    if (internal_xml_target(reader.*.source, target_start, target_end)) {
        if (!internal_matches(reader.*.source, target_start, "xml")) {
            return k_ra8_err_validation_failed;
        }
        return internal_declaration(reader, target_end, term);
    }
    err = priv_xml_raw(reader.*.source, target_end, term);
    if (err == k_ra8_ok) {
        reader.*.position = term + 2;
    }
    return err;
}

/// Validate and emit one CDATA payload. CDATA is accepted only within a
/// live element and aliases source.
fn internal_cdata(reader: [*c]xml_reader_t, event: [*c]xml_event_t) ra8_err_t {
    if (reader.*.stack_size == 0) {
        return k_ra8_err_validation_failed;
    }
    const content: usize = reader.*.position + k_priv_xml_cdata_open_bytes;
    var term: usize = 0;
    var err = internal_terminator(reader.*.source, reader.*.source_len, content, "]]>", &term);
    if (err != k_ra8_ok) {
        return err;
    }
    err = priv_xml_raw(reader.*.source, content, term);
    if (err != k_ra8_ok) {
        return err;
    }
    event.*.kind = k_xml_event_cdata;
    event.*.depth = reader.*.stack_size;
    event.*.markup = .{ .offset = @intCast(content), .length = @intCast(term - content) };
    reader.*.position = term + 3;
    return k_ra8_ok;
}

/// Dispatch comment, CDATA, DOCTYPE, or processing instruction. Fails
/// closed for unsupported declarations and malformed openers.
fn internal_special(reader: [*c]xml_reader_t, event: [*c]xml_event_t, out_emitted: *bool) ra8_err_t {
    const pos: usize = reader.*.position;
    out_emitted.* = false;
    if (((pos + 4) <= reader.*.source_len) and internal_matches(reader.*.source, pos, "<!--")) {
        return internal_comment(reader);
    }
    if (((pos + k_priv_xml_cdata_open_bytes) <= reader.*.source_len) and
        internal_matches(reader.*.source, pos, "<![CDATA["))
    {
        const err = internal_cdata(reader, event);
        out_emitted.* = (err == k_ra8_ok);
        return err;
    }
    if (((pos + k_priv_xml_doctype_open_bytes) <= reader.*.source_len) and
        internal_matches(reader.*.source, pos, "<!DOCTYPE"))
    {
        return priv_xml_doctype(reader);
    }
    // mcdc-deactivated: internal_special processing-instruction probe;
    // xml_reader_next rejects `(position + 1U) >= source_len` before
    // dispatching here, so `(pos + 2U) <= source_len` holds on every
    // reachable call and only the '?' byte test varies.
    if (((pos + 2) <= reader.*.source_len) and (reader.*.source[pos + 1] == '?')) {
        return internal_pi(reader);
    }
    return k_ra8_err_validation_failed;
}

/// Validate and emit one text run: validates UTF-8/entities, rejects raw
/// `]]>`, and enforces outer whitespace-only placement.
fn internal_text(reader: [*c]xml_reader_t, event: [*c]xml_event_t) ra8_err_t {
    const start: usize = reader.*.position;
    while ((reader.*.position < reader.*.source_len) and
        (reader.*.source[reader.*.position] != '<'))
    {
        reader.*.position += 1;
    }
    const span = xml_span_t{ .offset = @intCast(start), .length = @intCast(reader.*.position - start) };
    var decoded: usize = 0;
    const err = xml_decoded_size(reader.*.source, reader.*.source_len, span, &decoded);
    if (err != k_ra8_ok) {
        return err;
    }
    var i: usize = start;
    while ((i + 2) < reader.*.position) : (i += 1) {
        if ((reader.*.source[i] == ']') and (reader.*.source[i + 1] == ']') and
            (reader.*.source[i + 2] == '>'))
        {
            return k_ra8_err_validation_failed;
        }
    }
    if (reader.*.stack_size == 0) {
        var j: usize = start;
        while (j < reader.*.position) : (j += 1) {
            if (!internal_space(reader.*.source[j])) {
                return k_ra8_err_validation_failed;
            }
        }
    }
    event.*.kind = k_xml_event_text;
    event.*.depth = reader.*.stack_size;
    event.*.markup = span;
    return k_ra8_ok;
}

/// Initialise a pull pass over immutable bytes. A leading UTF-8 BOM is
/// consumed only when it begins at byte zero.
export fn xml_reader_init(reader: [*c]xml_reader_t, source: [*c]const u8, source_len: usize, workspace: [*c]xml_workspace_t) callconv(.c) ra8_err_t {
    if ((reader == null) or (source == null) or (workspace == null)) {
        return k_ra8_err_null_ptr;
    }
    if ((source_len == 0) or (source_len > std.math.maxInt(u32))) {
        return k_ra8_err_invalid_size;
    }
    reader.* = .{
        .source = source,
        .source_len = source_len,
        .position = 0,
        .workspace = workspace,
        .stack_size = 0,
        .root_count = 0,
        .root_closed = 0,
        .declaration_seen = 0,
        .doctype_seen = 0,
        .finished = 0,
    };
    if ((source_len >= 3) and (source[0] == k_priv_utf8_bom_first) and
        (source[1] == k_priv_utf8_bom_second) and (source[2] == k_priv_utf8_bom_third))
    {
        reader.*.position = 3;
    }
    return k_ra8_ok;
}

/// Return the next semantic event and validate syntax incrementally.
export fn xml_reader_next(reader: [*c]xml_reader_t, out_event: [*c]xml_event_t) callconv(.c) ra8_err_t {
    if ((reader == null) or (out_event == null)) {
        return k_ra8_err_null_ptr;
    }
    out_event.* = empty_event();
    while (reader.*.position < reader.*.source_len) {
        if (reader.*.source[reader.*.position] != '<') {
            return internal_text(reader, out_event);
        }
        if ((reader.*.position + 1) >= reader.*.source_len) {
            return k_ra8_err_validation_failed;
        }
        const next = reader.*.source[reader.*.position + 1];
        if ((next == '!') or (next == '?')) {
            var emitted = false;
            const err = internal_special(reader, out_event, &emitted);
            if ((err != k_ra8_ok) or emitted) {
                return err;
            }
            continue;
        }
        var end: usize = 0;
        const scan_err = internal_markup_end(reader.*.source, reader.*.source_len, reader.*.position, &end);
        if (scan_err != k_ra8_ok) {
            return scan_err;
        }
        if (next == '/') {
            return internal_end(reader, end, out_event);
        }
        return internal_start(reader, end, out_event);
    }
    // mcdc-deactivated: xml_reader_next end-of-document completeness gate;
    // root_closed is set by the only two ways the element stack can drain
    // back to zero after a root was started (a self-closing root in
    // internal_start, a matching end tag in internal_end), so
    // `stack_size == 0 && root_count == 1 && root_closed == 0` is
    // unreachable and the third condition cannot flip independently.
    if ((reader.*.stack_size != 0) or (reader.*.root_count != 1) or (reader.*.root_closed == 0)) {
        return k_ra8_err_validation_failed;
    }
    reader.*.finished = 1;
    return k_ra8_ok;
}

/// Validate a complete document before any consumer mutation.
export fn xml_validate(source: [*c]const u8, source_len: usize, workspace: [*c]xml_workspace_t) callconv(.c) ra8_err_t {
    var reader: xml_reader_t = undefined;
    var err = xml_reader_init(&reader, source, source_len, workspace);
    while (err == k_ra8_ok) {
        var event = empty_event();
        err = xml_reader_next(&reader, &event);
        if ((err != k_ra8_ok) or (event.kind == k_xml_event_none)) {
            break;
        }
    }
    return err;
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

test "reader walks a full document and emits the expected events" {
    const doc = "<?xml version=\"1.0\" encoding=\"UTF-8\"?><root a=\"1&amp;b\"><child/><!--c-->t&amp;x<![CDATA[raw]]><child>tail</child></root>";
    var workspace: xml_workspace_t = undefined;
    var reader: xml_reader_t = undefined;
    try testing.expectEqual(k_ra8_ok, xml_reader_init(&reader, doc.ptr, doc.len, &workspace));
    var event: xml_event_t = undefined;
    var started: usize = 0;
    var ended: usize = 0;
    var texts: usize = 0;
    var cdatas: usize = 0;
    while (true) {
        try testing.expectEqual(k_ra8_ok, xml_reader_next(&reader, &event));
        if (event.kind == k_xml_event_none) break;
        switch (event.kind) {
            k_xml_event_start => started += 1,
            k_xml_event_end => ended += 1,
            k_xml_event_text => texts += 1,
            k_xml_event_cdata => cdatas += 1,
            else => {},
        }
    }
    try testing.expectEqual(@as(usize, 3), started);
    try testing.expectEqual(@as(usize, 2), ended);
    try testing.expectEqual(@as(usize, 2), texts);
    try testing.expectEqual(@as(usize, 1), cdatas);
    try testing.expectEqual(k_ra8_ok, xml_validate(doc.ptr, doc.len, &workspace));
}

test "attribute iteration walks a start event in source order" {
    const doc = "<root a=\"1\" svg:x=\"&amp;\"/>";
    var workspace: xml_workspace_t = undefined;
    var reader: xml_reader_t = undefined;
    try testing.expectEqual(k_ra8_ok, xml_reader_init(&reader, doc.ptr, doc.len, &workspace));
    var event: xml_event_t = undefined;
    try testing.expectEqual(k_ra8_ok, xml_reader_next(&reader, &event));
    try testing.expectEqual(k_xml_event_start, event.kind);
    try testing.expectEqual(@as(c_ushort, 2), event.attribute_count);
    try testing.expectEqual(@as(u8, 1), event.self_closing);
    var cursor: xml_attr_cursor_t = undefined;
    xml_attr_begin(&event, &cursor);
    var attribute: xml_attribute_t = undefined;
    var has_value = false;
    try testing.expectEqual(k_ra8_ok, xml_attr_next(doc.ptr, doc.len, &event, &cursor, &attribute, &has_value));
    try testing.expect(has_value);
    try testing.expectEqual(@as(c_uint, 6), attribute.name.offset);
    try testing.expectEqual(@as(c_uint, 1), attribute.name.length);
    try testing.expectEqual(@as(c_uint, 9), attribute.value.offset);
    try testing.expectEqual(@as(c_uint, 1), attribute.value.length);
    try testing.expectEqual(k_ra8_ok, xml_attr_next(doc.ptr, doc.len, &event, &cursor, &attribute, &has_value));
    try testing.expect(has_value);
    try testing.expectEqual(@as(c_uint, 12), attribute.name.offset);
    try testing.expectEqual(@as(c_uint, 5), attribute.name.length);
    try testing.expectEqual(@as(c_uint, 19), attribute.value.offset);
    try testing.expectEqual(@as(c_uint, 5), attribute.value.length);
    try testing.expectEqual(k_ra8_ok, xml_attr_next(doc.ptr, doc.len, &event, &cursor, &attribute, &has_value));
    try testing.expect(!has_value);
}

test "qname subset accepts one colon and rejects the rest" {
    var out: usize = 0;
    const simple = "svg:rect";
    try testing.expectEqual(k_ra8_ok, priv_xml_qname(simple.ptr, simple.len, 0, &out));
    try testing.expectEqual(@as(usize, 8), out);
    const two_colons = "a:b:c";
    try testing.expectEqual(k_ra8_ok, priv_xml_qname(two_colons.ptr, two_colons.len, 0, &out));
    try testing.expectEqual(@as(usize, 3), out);
    const digit_start = "1abc";
    try testing.expectEqual(k_ra8_err_validation_failed, priv_xml_qname(digit_start.ptr, digit_start.len, 0, &out));
}

test "duplicate attributes and a literal less-than in a value fail closed" {
    var workspace: xml_workspace_t = undefined;
    const dup = "<r a=\"1\" a=\"2\"/>";
    try testing.expectEqual(k_ra8_err_validation_failed, xml_validate(dup.ptr, dup.len, &workspace));
    const lt = "<r a=\"<\"/>";
    try testing.expectEqual(k_ra8_err_validation_failed, xml_validate(lt.ptr, lt.len, &workspace));
}

test "DOCTYPE passthrough reaches the C validator through the seam" {
    var workspace: xml_workspace_t = undefined;
    const good = "<!DOCTYPE r SYSTEM \"r.dtd\"><r/>";
    try testing.expectEqual(k_ra8_ok, xml_validate(good.ptr, good.len, &workspace));
    const subset = "<!DOCTYPE r [ <!ENTITY x \"y\"> ]><r/>";
    try testing.expectEqual(k_ra8_err_validation_failed, xml_validate(subset.ptr, subset.len, &workspace));
}

test "BOM-prefixed declaration parses; a declaration off the front fails" {
    var workspace: xml_workspace_t = undefined;
    const bom_doc = "\xEF\xBB\xBF<?xml version=\"1.0\"?><r/>";
    try testing.expectEqual(k_ra8_ok, xml_validate(bom_doc.ptr, bom_doc.len, &workspace));
    const misplaced = "<!--x--><?xml version=\"1.0\"?><r/>";
    try testing.expectEqual(k_ra8_err_validation_failed, xml_validate(misplaced.ptr, misplaced.len, &workspace));
}

test "nesting and document-shape gates hold" {
    var workspace: xml_workspace_t = undefined;
    const mismatch = "<r></s>";
    try testing.expectEqual(k_ra8_err_validation_failed, xml_validate(mismatch.ptr, mismatch.len, &workspace));
    const unclosed = "<r>";
    try testing.expectEqual(k_ra8_err_validation_failed, xml_validate(unclosed.ptr, unclosed.len, &workspace));
    const ok_doc = "<r></r>";
    try testing.expectEqual(k_ra8_ok, xml_validate(ok_doc.ptr, ok_doc.len, &workspace));
    const stray = " x<r/>";
    try testing.expectEqual(k_ra8_err_validation_failed, xml_validate(stray.ptr, stray.len, &workspace));
    const cdata_top = "<r/>]]>";
    try testing.expectEqual(k_ra8_err_validation_failed, xml_validate(cdata_top.ptr, cdata_top.len, &workspace));
    const raw_cdata_close = "<r>]]></r>";
    try testing.expectEqual(k_ra8_err_validation_failed, xml_validate(raw_cdata_close.ptr, raw_cdata_close.len, &workspace));
}
