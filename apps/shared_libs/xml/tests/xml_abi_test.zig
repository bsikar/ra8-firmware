//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for `xml_abi.zig`.

const std = @import("std");
const testing = std.testing;
const sut = @import("xml_abi");

const k_ra8_err_no_mem = sut.k_ra8_err_no_mem;
const k_ra8_err_null_ptr = sut.k_ra8_err_null_ptr;
const k_ra8_err_validation_failed = sut.k_ra8_err_validation_failed;
const k_ra8_ok = sut.k_ra8_ok;
const k_xml_event_cdata = sut.k_xml_event_cdata;
const k_xml_event_end = sut.k_xml_event_end;
const k_xml_event_none = sut.k_xml_event_none;
const k_xml_event_start = sut.k_xml_event_start;
const k_xml_event_text = sut.k_xml_event_text;
const priv_xml_qname = sut.priv_xml_qname;
const priv_xml_raw = sut.priv_xml_raw;
const priv_xml_span_valid = sut.priv_xml_span_valid;
const xml_attr_begin = sut.xml_attr_begin;
const xml_attr_cursor_t = sut.xml_attr_cursor_t;
const xml_attr_next = sut.xml_attr_next;
const xml_attribute_t = sut.xml_attribute_t;
const xml_decode = sut.xml_decode;
const xml_decode_prefix = sut.xml_decode_prefix;
const xml_decoded_equal = sut.xml_decoded_equal;
const xml_decoded_size = sut.xml_decoded_size;
const xml_event_t = sut.xml_event_t;
const xml_reader_init = sut.xml_reader_init;
const xml_reader_next = sut.xml_reader_next;
const xml_reader_t = sut.xml_reader_t;
const xml_span_equal = sut.xml_span_equal;
const xml_span_local_equal = sut.xml_span_local_equal;
const xml_span_t = sut.xml_span_t;
const xml_validate = sut.xml_validate;
const xml_workspace_t = sut.xml_workspace_t;

// ---------------------------------------------------------------------------
// Zig-side unit tests. The library is pure, so these run with no seams.
// ---------------------------------------------------------------------------

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
