//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Zig-side tests for the resident XHTML serializer + text extractor port
//! (RA8FW-1250). The C suites keep covering the surface from the outside
//! through the unchanged C ABI; these exercise the port's own core paths over
//! built-in-memory RABOOK1 blobs: the shared leaf seams (block test, text
//! collapse, paragraph break), entity escaping and void elements, the
//! bounded DOM walks (nesting, siblings, overflow, the iteration guard), and
//! the two public entry points' gates (null args, chapter bounds).

const std = @import("std");
const xhtml = @import("book_xhtml.zig");
const wire = @import("book_stream_wire.zig");

const k_ra8_ok: u16 = 0;
const k_ra8_err_invalid_arg: u16 = 0x103;
const k_ra8_err_invalid_size: u16 = 0x105;
const k_ra8_err_null_ptr: u16 = 0x504;

const k_book_nil: u32 = 0xFFFFFFFF;

// ---- In-memory blob builder --------------------------------------------------
//
// Layout: [100-byte header][chapter table][node table][attr table][string
// pool]. Tables are host-order struct writes, matching how the port reads
// them (book_paged.zig's resident bind does the same memcpy). String offsets
// are named by pool index.

var g_blob: [8192]u8 = undefined;

const k_chapter_off: u32 = 128;
const k_node_off: u32 = 192;
const k_attr_off: u32 = 2048;
const k_string_off: u32 = 4096;

var g_pool_len: u32 = 0;
var g_attr_count: u32 = 0;

const TestNode = extern struct {
    kind: u8,
    reserved: u8,
    attr_count: u16,
    name_off: u32,
    text_off: u32,
    first_attr: u32,
    first_child: u32,
    next_sibling: u32,
};
const TestAttr = extern struct {
    name_off: u32,
    value_off: u32,
};
const TestChapter = extern struct {
    title_off: u32,
    href_off: u32,
    root_node: u32,
};

fn addString(s: []const u8) u32 {
    const off = g_pool_len;
    @memcpy(g_blob[k_string_off + off ..][0..s.len], s);
    g_blob[k_string_off + off + s.len] = 0;
    g_pool_len += @intCast(s.len + 1);
    return off;
}

/// One element or text node; string fields name pool entries. `attrs` lists
/// attribute (name pool index, value pool index) pairs; the node's
/// first_attr is the running attribute-table count when it is laid down.
const NodeSpec = struct {
    kind: u8,
    name: u32 = 0,
    text: u32 = 0,
    attrs: []const [2]u32 = &.{},
    first_child: u32 = k_book_nil,
    next_sibling: u32 = k_book_nil,
};

/// Build a one-chapter blob: `pool` holds the strings, `nodes` the DOM in
/// table order, `root` the chapter's root node index.
fn buildBlob(pool: []const []const u8, nodes: []const NodeSpec, root: u32) []u8 {
    @memset(&g_blob, 0);
    g_pool_len = 0;
    g_attr_count = 0;

    var offs: [64]u32 = undefined;
    for (pool, 0..) |s, i| offs[i] = addString(s);

    var znodes: [64]TestNode = undefined;
    for (nodes, 0..) |n, i| {
        znodes[i] = .{
            .kind = n.kind,
            .reserved = 0,
            .attr_count = @intCast(n.attrs.len),
            .name_off = offs[n.name],
            .text_off = offs[n.text],
            .first_attr = if (n.attrs.len > 0) g_attr_count else k_book_nil,
            .first_child = n.first_child,
            .next_sibling = n.next_sibling,
        };
        g_attr_count += @intCast(n.attrs.len);
    }

    var zattrs: [64]TestAttr = undefined;
    var ac: usize = 0;
    for (nodes) |n| {
        for (n.attrs) |a| {
            zattrs[ac] = .{ .name_off = offs[a[0]], .value_off = offs[a[1]] };
            ac += 1;
        }
    }

    var hdr: wire.BookHeader = std.mem.zeroes(wire.BookHeader);
    hdr.magic = "RABOOK1\x00".*;
    hdr.chapter_count = 1;
    hdr.chapter_off = k_chapter_off;
    hdr.node_count = @intCast(nodes.len);
    hdr.node_off = k_node_off;
    hdr.attr_count = @intCast(ac);
    hdr.attr_off = k_attr_off;
    hdr.string_off = k_string_off;
    hdr.string_size = g_pool_len;
    @memcpy(g_blob[0..100], std.mem.asBytes(&hdr));

    const chapters = [_]TestChapter{.{ .title_off = 0, .href_off = 0, .root_node = root }};
    @memcpy(g_blob[k_chapter_off..][0..12], std.mem.asBytes(&chapters));
    @memcpy(g_blob[k_node_off..][0 .. nodes.len * 24], std.mem.sliceAsBytes(znodes[0..nodes.len]));
    if (ac > 0) {
        @memcpy(g_blob[k_attr_off..][0 .. ac * 8], std.mem.sliceAsBytes(zattrs[0..ac]));
    }
    return g_blob[0..];
}

fn element(name: u32, attrs: []const [2]u32, first_child: u32, next_sibling: u32) NodeSpec {
    return .{ .kind = 0, .name = name, .attrs = attrs, .first_child = first_child, .next_sibling = next_sibling };
}
fn text(str: u32, next_sibling: u32) NodeSpec {
    return .{ .kind = 1, .text = str, .next_sibling = next_sibling };
}

// ---- Shared leaf seams --------------------------------------------------------

test "is_block table" {
    try std.testing.expectEqual(true, xhtml.priv_book_is_block("p"));
    try std.testing.expectEqual(true, xhtml.priv_book_is_block("h6"));
    try std.testing.expectEqual(true, xhtml.priv_book_is_block("figcaption"));
    try std.testing.expectEqual(false, xhtml.priv_book_is_block("span"));
    try std.testing.expectEqual(false, xhtml.priv_book_is_block(""));
}

test "emit_text collapses whitespace and carries at_break" {
    var buf: [64]u8 = undefined;
    var pos: usize = 0;
    var at_break: bool = true;
    try std.testing.expectEqual(true, xhtml.priv_book_emit_text(&buf, buf.len, &pos, "  a\t\nb  ", &at_break));
    try std.testing.expectEqualStrings("a b ", buf[0..pos]);
    try std.testing.expectEqual(true, at_break);

    // Leading whitespace is dropped while at_break; a non-ws byte clears it.
    pos = 0;
    at_break = false;
    try std.testing.expectEqual(true, xhtml.priv_book_emit_text(&buf, buf.len, &pos, " x", &at_break));
    try std.testing.expectEqualStrings(" x", buf[0..pos]);
    try std.testing.expectEqual(false, at_break); // trailing non-ws byte clears it
}

test "emit_text stops on overflow with partial output" {
    var buf: [3]u8 = undefined;
    var pos: usize = 0;
    var at_break: bool = false;
    try std.testing.expectEqual(false, xhtml.priv_book_emit_text(&buf, buf.len, &pos, "abcd", &at_break));
    try std.testing.expectEqual(@as(usize, 3), pos);
    try std.testing.expectEqualStrings("abc", buf[0..pos]);
}

test "emit_break trims spaces and collapses consecutive breaks" {
    var buf: [64]u8 = undefined;
    var pos: usize = 0;
    var at_break: bool = false;
    @memcpy(buf[0..3], "a  ");
    pos = 3;
    try std.testing.expectEqual(true, xhtml.priv_book_emit_break(&buf, buf.len, &pos, &at_break));
    try std.testing.expectEqualStrings("a\n", buf[0..pos]);
    try std.testing.expectEqual(true, at_break);
    // A break right after a newline emits nothing further.
    try std.testing.expectEqual(true, xhtml.priv_book_emit_break(&buf, buf.len, &pos, &at_break));
    try std.testing.expectEqualStrings("a\n", buf[0..pos]);
}

// ---- XHTML serialisation -------------------------------------------------------

test "chapter_to_xhtml simple element with text" {
    // <p>hello</p>
    const blob = buildBlob(&.{ "p", "hello" }, &.{
        element(0, &.{}, 1, k_book_nil),
        text(1, k_book_nil),
    }, 0);
    var out: [256]u8 = undefined;
    var out_len: usize = 0;
    const err = xhtml.book_chapter_to_xhtml(blob.ptr, 0, &out, out.len, &out_len);
    try std.testing.expectEqual(k_ra8_ok, err);
    try std.testing.expectEqualStrings("<p>hello</p>", out[0..out_len]);
}

test "chapter_to_xhtml void elements self-close" {
    // <div><br/><img src="x.png"/></div>
    const blob = buildBlob(&.{ "div", "br", "img", "src", "x.png" }, &.{
        element(0, &.{}, 1, k_book_nil),
        element(1, &.{}, k_book_nil, 2),
        element(2, &.{.{ 3, 4 }}, k_book_nil, k_book_nil),
    }, 0);
    var out: [256]u8 = undefined;
    var out_len: usize = 0;
    const err = xhtml.book_chapter_to_xhtml(blob.ptr, 0, &out, out.len, &out_len);
    try std.testing.expectEqual(k_ra8_ok, err);
    try std.testing.expectEqualStrings("<div><br/><img src=\"x.png\"/></div>", out[0..out_len]);
}

test "chapter_to_xhtml escapes text and attribute values" {
    // <span class="a & b">say "hi" <x>&</x></span> — the quote stays literal
    // in content but is escaped inside the attribute value.
    const blob = buildBlob(
        &.{ "span", "class", "a & b", "say \"hi\" <x>&", "p" },
        &.{
            element(0, &.{.{ 1, 2 }}, 1, k_book_nil),
            text(3, k_book_nil),
            element(4, &.{}, k_book_nil, k_book_nil),
        },
        0,
    );
    var out: [256]u8 = undefined;
    var out_len: usize = 0;
    const err = xhtml.book_chapter_to_xhtml(blob.ptr, 0, &out, out.len, &out_len);
    try std.testing.expectEqual(k_ra8_ok, err);
    try std.testing.expectEqualStrings(
        "<span class=\"a &amp; b\">say \"hi\" &lt;x&gt;&amp;</span>",
        out[0..out_len],
    );
}

test "chapter_to_xhtml nests elements and walks siblings" {
    // <div><p>one</p><p>two</p></div>
    const blob = buildBlob(&.{ "div", "p", "one", "two" }, &.{
        element(0, &.{}, 1, k_book_nil),
        element(1, &.{}, 2, 3),
        text(2, k_book_nil),
        element(1, &.{}, 4, k_book_nil),
        text(3, k_book_nil),
    }, 0);
    var out: [256]u8 = undefined;
    var out_len: usize = 0;
    const err = xhtml.book_chapter_to_xhtml(blob.ptr, 0, &out, out.len, &out_len);
    try std.testing.expectEqual(k_ra8_ok, err);
    try std.testing.expectEqualStrings("<div><p>one</p><p>two</p></div>", out[0..out_len]);
}

// ---- Plain-text extraction -------------------------------------------------------

test "chapter_text collapses blocks to single breaks" {
    // <div><p>one</p><p>two</p></div> -> "\none\ntwo"
    const blob = buildBlob(&.{ "div", "p", "one", "two" }, &.{
        element(0, &.{}, 1, k_book_nil),
        element(1, &.{}, 2, 3),
        text(2, k_book_nil),
        element(1, &.{}, 4, k_book_nil),
        text(3, k_book_nil),
    }, 0);
    var out: [256]u8 = undefined;
    var out_len: usize = 0;
    const err = xhtml.book_chapter_text(blob.ptr, 0, &out, out.len, &out_len);
    try std.testing.expectEqual(k_ra8_ok, err);
    try std.testing.expectEqualStrings("\none\ntwo", out[0..out_len]);
}

test "chapter_text collapses inline whitespace" {
    // <p>a  b\t c</p> -> "a b c" (leading whitespace dropped while at_break).
    const blob = buildBlob(&.{ "p", "a  b\t c" }, &.{
        element(0, &.{}, 1, k_book_nil),
        text(1, k_book_nil),
    }, 0);
    var out: [256]u8 = undefined;
    var out_len: usize = 0;
    const err = xhtml.book_chapter_text(blob.ptr, 0, &out, out.len, &out_len);
    try std.testing.expectEqual(k_ra8_ok, err);
    try std.testing.expectEqualStrings("\na b c", out[0..out_len]); // the root block emits its break first
}

// ---- Bounds, overflow and guard paths ---------------------------------------------

test "walk overflow returns invalid_size and leaves out_len alone" {
    const blob = buildBlob(&.{ "p", "hello" }, &.{
        element(0, &.{}, 1, k_book_nil),
        text(1, k_book_nil),
    }, 0);
    var out: [5]u8 = undefined;
    var out_len: usize = 999;
    const err = xhtml.book_chapter_to_xhtml(blob.ptr, 0, &out, out.len, &out_len);
    try std.testing.expectEqual(k_ra8_err_invalid_size, err);
    try std.testing.expectEqual(@as(usize, 999), out_len);

    const err2 = xhtml.book_chapter_text(blob.ptr, 0, &out, out.len, &out_len);
    try std.testing.expectEqual(k_ra8_err_invalid_size, err2);
}

test "chapter index out of range returns invalid_arg" {
    const blob = buildBlob(&.{"p"}, &.{element(0, &.{}, k_book_nil, k_book_nil)}, 0);
    var out: [256]u8 = undefined;
    var out_len: usize = 0;
    try std.testing.expectEqual(
        k_ra8_err_invalid_arg,
        xhtml.book_chapter_to_xhtml(blob.ptr, 1, &out, out.len, &out_len),
    );
    try std.testing.expectEqual(
        k_ra8_err_invalid_arg,
        xhtml.book_chapter_text(blob.ptr, 1, &out, out.len, &out_len),
    );
}

test "null arguments return null_ptr" {
    const blob = buildBlob(&.{"p"}, &.{element(0, &.{}, k_book_nil, k_book_nil)}, 0);
    var out: [256]u8 = undefined;
    var out_len: usize = 0;
    try std.testing.expectEqual(k_ra8_err_null_ptr, xhtml.book_chapter_to_xhtml(null, 0, &out, out.len, &out_len));
    try std.testing.expectEqual(k_ra8_err_null_ptr, xhtml.book_chapter_to_xhtml(blob.ptr, 0, null, out.len, &out_len));
    try std.testing.expectEqual(k_ra8_err_null_ptr, xhtml.book_chapter_to_xhtml(blob.ptr, 0, &out, out.len, null));
    try std.testing.expectEqual(k_ra8_err_null_ptr, xhtml.book_chapter_text(null, 0, &out, out.len, &out_len));
    try std.testing.expectEqual(k_ra8_err_null_ptr, xhtml.book_chapter_text(blob.ptr, 0, null, out.len, &out_len));
    try std.testing.expectEqual(k_ra8_err_null_ptr, xhtml.book_chapter_text(blob.ptr, 0, &out, out.len, null));
}

test "iteration guard stops a cyclic DOM" {
    // node 0 is its own next_sibling: unbounded without the guard.
    const blob = buildBlob(&.{"div"}, &.{
        element(0, &.{}, k_book_nil, 0),
    }, 0);
    var out: [256]u8 = undefined;
    var out_len: usize = 0;
    try std.testing.expectEqual(
        k_ra8_err_invalid_size,
        xhtml.book_chapter_to_xhtml(blob.ptr, 0, &out, out.len, &out_len),
    );
}
