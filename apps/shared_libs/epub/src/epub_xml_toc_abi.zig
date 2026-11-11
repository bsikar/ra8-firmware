//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The bounded NCX and EPUB 3 nav table-of-contents consumers
//! (RA8FW-1166), ported from src/epub_xml_toc.c behind the unchanged
//! internal headers. The shim helpers (priv_epub_xml_*) and the ra8_xml
//! reader reach through the archive link, so the archive stays pullable on
//! its own: a consumer that never parses a navigation document never
//! pulls the TOC TU.
const std = @import("std");

const ra8_err_t = c_uint;

const k_ra8_ok: ra8_err_t = 0;
const k_ra8_err_no_mem: ra8_err_t = 0x102;
const k_ra8_err_invalid_size: ra8_err_t = 0x105;
const k_ra8_err_validation_failed: ra8_err_t = 0x501;
const k_ra8_err_null_ptr: ra8_err_t = 0x504;

const k_epub_max_manifest = 96;
const k_epub_id_len = 64;
const k_epub_media_len = 48;
const k_epub_zip_archive_bytes = 256;
const k_epub_max_chapters = 64;
const k_epub_max_toc = 64;
const k_epub_max_path_len = 192;
const k_epub_meta_len = 128;
const k_epub_max_fonts = 8;

// ---- xml.h (apps/shared_libs/xml/inc/xml.h, ra8_xml stays linked) ----------
const xml_span_t = extern struct { offset: u32, length: u32 };
const xml_frame_t = extern struct { name_offset: u32, name_length: u16, consumer: u16 };
const k_xml_workspace_frames = 512;
const xml_workspace_t = extern struct { frames: [k_xml_workspace_frames]xml_frame_t };
const xml_event_t = extern struct {
    markup: xml_span_t,
    name: xml_span_t,
    depth: u16,
    attribute_count: u16,
    kind: u8,
    self_closing: u8,
};
const xml_reader_t = extern struct {
    source: ?[*]const u8,
    source_len: usize,
    position: usize,
    workspace: ?*xml_workspace_t,
    stack_size: u16,
    root_count: u8,
    root_closed: u8,
    declaration_seen: u8,
    doctype_seen: u8,
    finished: u8,
};
const k_xml_event_none: u8 = 0;
const k_xml_event_start: u8 = 1;
const k_xml_event_end: u8 = 2;
const k_xml_event_text: u8 = 3;

extern fn xml_validate(source: ?[*]const u8, source_len: usize, workspace: ?*xml_workspace_t) callconv(.c) ra8_err_t;
extern fn xml_reader_next(reader: ?*xml_reader_t, out_event: ?*xml_event_t) callconv(.c) ra8_err_t;
extern fn xml_span_local_equal(source: ?[*]const u8, source_len: usize, span: xml_span_t, literal: [*:0]const u8) callconv(.c) bool;

// ---- the epub book and workspace (epub.h, exact C layout) ------------------
const epub_xml_workspace_t = extern struct {
    reader: xml_workspace_t,
    references: [k_epub_max_chapters]xml_span_t,
    legacy_cover_id: xml_span_t,
    reference_count: u16,
};
const epub_miniz_arena_t = extern struct { base: ?[*]u8, capacity: usize, initialized: u8 };
const epub_miniz_workspace_t = extern struct { bytes: [163840]u8 align(16) };
const epub_stream_read_fn = *const fn (?*anyopaque, u64, ?*anyopaque, u32, ?*u32) callconv(.c) ra8_err_t;
const epub_stream_media_t = extern struct { read: ?epub_stream_read_fn, ctx: ?*anyopaque, size: u64 };
const epub_container_result_t = extern struct { opf_path: [k_epub_max_path_len]u8 };
const epub_toc_entry_t = extern struct { title: [k_epub_meta_len]u8, href: [k_epub_max_path_len]u8, depth: u8 };
const epub_manifest_item_t = extern struct { id: [k_epub_id_len]u8, href: [k_epub_max_path_len]u8, media_type: [k_epub_media_len]u8 };
const epub_book_t = extern struct {
    zip_bytes: ?[*]const u8,
    zip_size: usize,
    zip_archive_storage: [k_epub_zip_archive_bytes]u8 align(16),
    zip_archive_active: u8,
    miniz_arena: epub_miniz_arena_t,
    miniz_workspace: epub_miniz_workspace_t,
    xml_workspace: epub_xml_workspace_t,
    stream_media: epub_stream_media_t,
    chapter_count: u16,
    chapter_paths: [k_epub_max_chapters][k_epub_max_path_len]u8,
    title: [k_epub_meta_len]u8,
    author: [k_epub_meta_len]u8,
    language: [k_epub_meta_len]u8,
    identifier: [k_epub_meta_len]u8,
    cover_path: [k_epub_max_path_len]u8,
    embedded_font_count: u16,
    embedded_font_paths: [k_epub_max_fonts][k_epub_max_path_len]u8,
    manifest_count: u16,
    manifest: [k_epub_max_manifest]epub_manifest_item_t,
    opf_dir: [k_epub_max_path_len]u8,
    toc_path: [k_epub_max_path_len]u8,
    toc_kind: u8,
    toc_count: u16,
    toc: [k_epub_max_toc]epub_toc_entry_t,
    font_data: ?[*]const u8,
    font_size: usize,
    in_use: u8,
};

/// Optional attribute span (epub_xml_consumer_internal.h priv_attr_t).
const priv_attr_t = extern struct { span: xml_span_t, present: bool };

extern fn priv_epub_xml_attr(source: ?[*]const u8, source_len: usize, event: ?*const xml_event_t, name: [*:0]const u8) callconv(.c) priv_attr_t;
extern fn priv_epub_xml_attr_contains(source: ?[*]const u8, source_len: usize, attribute: priv_attr_t, needle: [*:0]const u8) callconv(.c) bool;
extern fn priv_epub_xml_frame_name(reader: ?*const xml_reader_t, frame: u16) callconv(.c) xml_span_t;
extern fn priv_epub_xml_ancestor_marker(reader: ?*const xml_reader_t, depth: u16, levels: u16) callconv(.c) u16;
extern fn priv_epub_xml_ancestor_frame(depth: u16, levels: u16, out_frame: ?*u16) callconv(.c) bool;
extern fn priv_epub_xml_direct_child(child: u16, parent: u16) callconv(.c) bool;
extern fn priv_epub_xml_copy(source: ?[*]const u8, source_len: usize, span: xml_span_t, destination: ?[*]u8, capacity: usize) callconv(.c) void;
extern fn priv_epub_xml_reader(reader: ?*xml_reader_t, source: ?[*]const u8, length: usize, workspace: ?*epub_xml_workspace_t) callconv(.c) ra8_err_t;
extern fn priv_epub_xml_find(source: ?[*]const u8, length: usize, workspace: ?*epub_xml_workspace_t, local: [*:0]const u8, use_parent: bool, parent_offset: u32, parent_depth: u16, out: ?*xml_event_t) callconv(.c) ra8_err_t;

comptime {
    // 64-bit host layout checks; the mirrors are built from the same width
    // generic types as the C headers, so the 32-bit AAPCS layout shrinks with
    // @sizeOf(usize) the same way the C structs do.
    if (@sizeOf(usize) == 8) {
        if (@sizeOf(epub_book_t) != 233472) @compileError("epub_book_t mirror diverges from the C layout");
        if (@sizeOf(xml_reader_t) != 40) @compileError("xml_reader_t mirror diverges from the C layout");
    }
    if (@sizeOf(epub_toc_entry_t) != 321) @compileError("epub_toc_entry_t mirror diverges from the C layout");
}

const priv_ncx_ctx_t = struct {
    source: [*]const u8,
    source_len: usize,
    book: *epub_book_t,
    reader: *xml_reader_t,
    selected: xml_event_t,
    active: bool,
};
const priv_nav_ctx_t = struct {
    source: [*]const u8,
    source_len: usize,
    book: *epub_book_t,
    reader: *xml_reader_t,
    selected: xml_event_t,
    active: bool,
    saw_ol: bool,
};

// ---- internal helpers ------------------------------------------------------

fn internal_ancestor_name(reader: *const xml_reader_t, depth: u16, levels: u16) xml_span_t {
    var frame: u16 = 0;
    return if (priv_epub_xml_ancestor_frame(depth, levels, &frame))
        priv_epub_xml_frame_name(reader, frame)
    else
        .{ .offset = 0, .length = 0 };
}

fn internal_ancestor_depth(source: [*]const u8, source_len: usize, reader: *const xml_reader_t, depth: u16, local: [*:0]const u8) u8 {
    var count: u8 = 0;
    var i: u16 = 0;
    while (i < depth) : (i += 1) {
        if (xml_span_local_equal(source, source_len, priv_epub_xml_frame_name(reader, i), local)) {
            count += 1;
        }
    }
    return count;
}

fn internal_toc_reserve(source: [*]const u8, source_len: usize, event: *const xml_event_t, reader: *xml_reader_t, book: *epub_book_t, local: [*:0]const u8) u16 {
    var marker: u16 = 0;
    if (book.toc_count < k_epub_max_toc) {
        const slot = book.toc_count;
        book.toc_count += 1;
        book.toc[slot] = std.mem.zeroes(epub_toc_entry_t);
        book.toc[slot].depth = internal_ancestor_depth(source, source_len, reader, event.depth, local);
        marker = slot + 1;
    }
    if (event.self_closing == 0) {
        reader.workspace.?.frames[event.depth].consumer = marker;
    }
    return marker;
}

fn internal_toc_marker(reader: *const xml_reader_t, depth: u16) u16 {
    var level = depth;
    while (level > 0) {
        level -= 1;
        const marker = reader.workspace.?.frames[level].consumer;
        if (marker != 0) {
            return marker;
        }
    }
    return 0;
}

fn internal_toc_capacity(source: [*]const u8, length: usize, workspace: *epub_xml_workspace_t, selected: *const xml_event_t, entry_local: [*:0]const u8, require_list: bool) ra8_err_t {
    var reader: xml_reader_t = std.mem.zeroes(xml_reader_t);
    var err = priv_epub_xml_reader(&reader, source, length, workspace);
    var active = false;
    var enabled = !require_list;
    var count: u16 = 0;
    while (err == k_ra8_ok) {
        var event: xml_event_t = std.mem.zeroes(xml_event_t);
        err = xml_reader_next(&reader, &event);
        if ((err != k_ra8_ok) or (event.kind == k_xml_event_none)) {
            break;
        }
        if (!active and (event.kind == k_xml_event_start) and
            (event.markup.offset == selected.markup.offset))
        {
            active = true;
        } else if (active and (event.kind == k_xml_event_start) and require_list and
            priv_epub_xml_direct_child(event.depth, selected.depth) and
            xml_span_local_equal(source, length, event.name, "ol"))
        {
            enabled = true;
        } else if (active and enabled and (event.kind == k_xml_event_start) and
            xml_span_local_equal(source, length, event.name, entry_local))
        {
            if (count >= k_epub_max_toc) {
                return k_ra8_err_no_mem;
            }
            count += 1;
        } else if (active and (event.kind == k_xml_event_end) and
            (event.depth == selected.depth))
        {
            return k_ra8_ok;
        }
    }
    return err;
}

fn internal_ncx_event(ctx: *priv_ncx_ctx_t, event: *const xml_event_t) void {
    if (!ctx.active and (event.kind == k_xml_event_start) and
        (event.markup.offset == ctx.selected.markup.offset))
    {
        ctx.active = true;
    } else if (ctx.active and (event.kind == k_xml_event_start) and
        xml_span_local_equal(ctx.source, ctx.source_len, event.name, "navPoint"))
    {
        _ = internal_toc_reserve(ctx.source, ctx.source_len, event, ctx.reader, ctx.book, "navPoint");
    } else if (ctx.active and (event.kind == k_xml_event_start) and
        xml_span_local_equal(ctx.source, ctx.source_len, event.name, "content"))
    {
        const marker = internal_toc_marker(ctx.reader, event.depth);
        const src = priv_epub_xml_attr(ctx.source, ctx.source_len, event, "src");
        if ((marker != 0) and src.present) {
            priv_epub_xml_copy(ctx.source, ctx.source_len, src.span, &ctx.book.toc[marker - 1].href, k_epub_max_path_len);
        }
    } else if ((event.kind == k_xml_event_text) and (event.depth >= 3) and
        xml_span_local_equal(ctx.source, ctx.source_len, internal_ancestor_name(ctx.reader, event.depth, 1), "text") and
        xml_span_local_equal(ctx.source, ctx.source_len, internal_ancestor_name(ctx.reader, event.depth, 2), "navLabel"))
    {
        var nav_label: u16 = 0;
        const found = priv_epub_xml_ancestor_frame(event.depth, 2, &nav_label);
        const marker = if (found) internal_toc_marker(ctx.reader, nav_label) else 0;
        if ((marker != 0) and (ctx.book.toc[marker - 1].title[0] == 0)) {
            priv_epub_xml_copy(ctx.source, ctx.source_len, event.markup, &ctx.book.toc[marker - 1].title, k_epub_meta_len);
        }
    } else if ((event.kind == k_xml_event_end) and ctx.active and
        (event.depth == ctx.selected.depth))
    {
        ctx.active = false;
    }
}

pub export fn priv_epub_xml_parse_ncx(xml_bytes: ?[*]const u8, xml_len: usize, book: ?*epub_book_t) callconv(.c) ra8_err_t {
    if ((xml_bytes == null) or (book == null)) {
        return k_ra8_err_null_ptr;
    }
    if (xml_len == 0) {
        return k_ra8_err_invalid_size;
    }
    const source = xml_bytes.?;
    const target = book.?;
    var err = xml_validate(source, xml_len, &target.xml_workspace.reader);
    var selected: xml_event_t = std.mem.zeroes(xml_event_t);
    if (err == k_ra8_ok) {
        err = priv_epub_xml_find(source, xml_len, &target.xml_workspace, "navMap", false, 0, 0, &selected);
    }
    if (err == k_ra8_ok) {
        err = internal_toc_capacity(source, xml_len, &target.xml_workspace, &selected, "navPoint", false);
    }
    if (err != k_ra8_ok) {
        return err;
    }
    target.toc_count = 0;
    var reader: xml_reader_t = std.mem.zeroes(xml_reader_t);
    err = priv_epub_xml_reader(&reader, source, xml_len, &target.xml_workspace);
    var ctx: priv_ncx_ctx_t = .{
        .source = source,
        .source_len = xml_len,
        .book = target,
        .reader = &reader,
        .selected = selected,
        .active = false,
    };
    while (err == k_ra8_ok) {
        var event: xml_event_t = std.mem.zeroes(xml_event_t);
        err = xml_reader_next(&reader, &event);
        if ((err != k_ra8_ok) or (event.kind == k_xml_event_none)) {
            break;
        }
        internal_ncx_event(&ctx, &event);
    }
    return err;
}

fn internal_select_nav(source: [*]const u8, length: usize, workspace: *epub_xml_workspace_t, out: *xml_event_t) ra8_err_t {
    var reader: xml_reader_t = std.mem.zeroes(xml_reader_t);
    var err = priv_epub_xml_reader(&reader, source, length, workspace);
    var fallback: xml_event_t = std.mem.zeroes(xml_event_t);
    while (err == k_ra8_ok) {
        var event: xml_event_t = std.mem.zeroes(xml_event_t);
        err = xml_reader_next(&reader, &event);
        if ((err != k_ra8_ok) or (event.kind == k_xml_event_none)) {
            break;
        }
        if ((event.kind == k_xml_event_start) and (event.depth > 0) and
            xml_span_local_equal(source, length, event.name, "nav"))
        {
            if (fallback.kind == k_xml_event_none) {
                fallback = event;
            }
            if (priv_epub_xml_attr_contains(source, length, priv_epub_xml_attr(source, length, &event, "epub:type"), "toc")) {
                out.* = event;
                return k_ra8_ok;
            }
        }
    }
    if ((err == k_ra8_ok) and (fallback.kind != k_xml_event_none)) {
        out.* = fallback;
        return k_ra8_ok;
    }
    return if (err == k_ra8_ok) k_ra8_err_validation_failed else err;
}

fn internal_nav_has_list(source: [*]const u8, length: usize, workspace: *epub_xml_workspace_t, selected: *const xml_event_t) ra8_err_t {
    var reader: xml_reader_t = std.mem.zeroes(xml_reader_t);
    var err = priv_epub_xml_reader(&reader, source, length, workspace);
    var active = false;
    while (err == k_ra8_ok) {
        var event: xml_event_t = std.mem.zeroes(xml_event_t);
        err = xml_reader_next(&reader, &event);
        if ((err != k_ra8_ok) or (event.kind == k_xml_event_none)) {
            break;
        }
        if (!active and (event.kind == k_xml_event_start) and
            (event.markup.offset == selected.markup.offset))
        {
            active = true;
        } else if (active and (event.kind == k_xml_event_start) and
            priv_epub_xml_direct_child(event.depth, selected.depth) and
            xml_span_local_equal(source, length, event.name, "ol"))
        {
            return k_ra8_ok;
        } else if (active and (event.kind == k_xml_event_end) and
            (event.depth == selected.depth))
        {
            return k_ra8_err_validation_failed;
        }
    }
    return if (err == k_ra8_ok) k_ra8_err_validation_failed else err;
}

fn internal_nav_event_start(ctx: *priv_nav_ctx_t, event: *const xml_event_t) void {
    if (priv_epub_xml_direct_child(event.depth, ctx.selected.depth) and
        xml_span_local_equal(ctx.source, ctx.source_len, event.name, "ol"))
    {
        ctx.saw_ol = true;
        return;
    }
    if (ctx.saw_ol and xml_span_local_equal(ctx.source, ctx.source_len, event.name, "li")) {
        _ = internal_toc_reserve(ctx.source, ctx.source_len, event, ctx.reader, ctx.book, "li");
        return;
    }
    if (!(ctx.saw_ol and (xml_span_local_equal(ctx.source, ctx.source_len, event.name, "a") or
        xml_span_local_equal(ctx.source, ctx.source_len, event.name, "span"))))
    {
        return;
    }
    const marker = internal_toc_marker(ctx.reader, event.depth);
    if ((marker == 0) or (event.self_closing != 0)) {
        return;
    }
    ctx.reader.workspace.?.frames[event.depth].consumer = marker;
    if (!xml_span_local_equal(ctx.source, ctx.source_len, event.name, "a")) {
        return;
    }
    const href = priv_epub_xml_attr(ctx.source, ctx.source_len, event, "href");
    if (href.present) {
        priv_epub_xml_copy(ctx.source, ctx.source_len, href.span, &ctx.book.toc[marker - 1].href, k_epub_max_path_len);
    }
    ctx.book.toc[marker - 1].title[0] = 0;
}

fn internal_nav_event(ctx: *priv_nav_ctx_t, event: *const xml_event_t) void {
    if (!ctx.active and (event.kind == k_xml_event_start) and
        (event.markup.offset == ctx.selected.markup.offset))
    {
        ctx.active = true;
        return;
    }
    if (!ctx.active) {
        return;
    }
    if (event.kind == k_xml_event_start) {
        internal_nav_event_start(ctx, event);
    } else if ((event.kind == k_xml_event_text) and (event.depth > 0)) {
        const parent = internal_ancestor_name(ctx.reader, event.depth, 1);
        const marker = priv_epub_xml_ancestor_marker(ctx.reader, event.depth, 1);
        if ((xml_span_local_equal(ctx.source, ctx.source_len, parent, "a") or
            xml_span_local_equal(ctx.source, ctx.source_len, parent, "span")) and
            (marker != 0) and (ctx.book.toc[marker - 1].title[0] == 0))
        {
            priv_epub_xml_copy(ctx.source, ctx.source_len, event.markup, &ctx.book.toc[marker - 1].title, k_epub_meta_len);
        }
    } else if ((event.kind == k_xml_event_end) and (event.depth == ctx.selected.depth)) {
        ctx.active = false;
    }
}

pub export fn priv_epub_xml_parse_nav(xml_bytes: ?[*]const u8, xml_len: usize, book: ?*epub_book_t) callconv(.c) ra8_err_t {
    if ((xml_bytes == null) or (book == null)) {
        return k_ra8_err_null_ptr;
    }
    if (xml_len == 0) {
        return k_ra8_err_invalid_size;
    }
    const source = xml_bytes.?;
    const target = book.?;
    var err = xml_validate(source, xml_len, &target.xml_workspace.reader);
    var selected: xml_event_t = std.mem.zeroes(xml_event_t);
    if (err == k_ra8_ok) {
        err = internal_select_nav(source, xml_len, &target.xml_workspace, &selected);
    }
    if (err == k_ra8_ok) {
        err = internal_nav_has_list(source, xml_len, &target.xml_workspace, &selected);
    }
    if (err == k_ra8_ok) {
        err = internal_toc_capacity(source, xml_len, &target.xml_workspace, &selected, "li", true);
    }
    if (err != k_ra8_ok) {
        return err;
    }
    target.toc_count = 0;
    var reader: xml_reader_t = std.mem.zeroes(xml_reader_t);
    err = priv_epub_xml_reader(&reader, source, xml_len, &target.xml_workspace);
    var ctx: priv_nav_ctx_t = .{
        .source = source,
        .source_len = xml_len,
        .book = target,
        .reader = &reader,
        .selected = selected,
        .active = false,
        .saw_ol = false,
    };
    while (err == k_ra8_ok) {
        var event: xml_event_t = std.mem.zeroes(xml_event_t);
        err = xml_reader_next(&reader, &event);
        if ((err != k_ra8_ok) or (event.kind == k_xml_event_none)) {
            break;
        }
        internal_nav_event(&ctx, &event);
    }
    if ((err != k_ra8_ok) or !ctx.saw_ol) {
        return if (err == k_ra8_ok) k_ra8_err_validation_failed else err;
    }
    return k_ra8_ok;
}

// ---- tests -----------------------------------------------------------------

const testing = std.testing;

fn expect_toc_entry(book: *const epub_book_t, index: usize, title: []const u8, href: []const u8) !void {
    try testing.expectEqualStrings(title, std.mem.sliceTo(&book.toc[index].title, 0));
    try testing.expectEqualStrings(href, std.mem.sliceTo(&book.toc[index].href, 0));
}

test "parse_ncx walks the navMap in document order" {
    const doc =
        "<?xml version=\"1.0\"?><ncx><head/><navMap>" ++
        "<navPoint id=\"n1\"><navLabel><text>Intro</text></navLabel><content src=\"ch1.xhtml\"/></navPoint>" ++
        "<navPoint id=\"n2\"><navLabel><text>Next</text></navLabel><content src=\"ch2.xhtml\"/></navPoint>" ++
        "</navMap></ncx>";
    var book: epub_book_t = std.mem.zeroes(epub_book_t);
    try testing.expectEqual(k_ra8_ok, priv_epub_xml_parse_ncx(doc, doc.len, &book));
    try testing.expectEqual(@as(u16, 2), book.toc_count);
    try expect_toc_entry(&book, 0, "Intro", "ch1.xhtml");
    try expect_toc_entry(&book, 1, "Next", "ch2.xhtml");
    try testing.expectEqual(book.toc[0].depth, book.toc[1].depth);
}

test "parse_ncx keeps nested navPoints one level deeper" {
    const doc =
        "<?xml version=\"1.0\"?><ncx><navMap>" ++
        "<navPoint id=\"p\"><navLabel><text>Part</text></navLabel>" ++
        "<navPoint id=\"c\"><navLabel><text>Chapter</text></navLabel><content src=\"c1.xhtml\"/></navPoint>" ++
        "<content src=\"part.xhtml\"/></navPoint>" ++
        "</navMap></ncx>";
    var book: epub_book_t = std.mem.zeroes(epub_book_t);
    try testing.expectEqual(k_ra8_ok, priv_epub_xml_parse_ncx(doc, doc.len, &book));
    try testing.expectEqual(@as(u16, 2), book.toc_count);
    try expect_toc_entry(&book, 0, "Part", "part.xhtml");
    try expect_toc_entry(&book, 1, "Chapter", "c1.xhtml");
    try testing.expectEqual(book.toc[0].depth + 1, book.toc[1].depth);
}

test "parse_ncx refuses a navMap that overflows the fixed TOC capacity" {
    var doc: [16384]u8 = undefined;
    const prologue = "<?xml version=\"1.0\"?><ncx><navMap>";
    @memcpy(doc[0..prologue.len], prologue);
    var pos = prologue.len;
    for (0..k_epub_max_toc + 1) |i| {
        const written = try std.fmt.bufPrint(doc[pos..], "<navPoint id=\"n{d}\"><navLabel><text>T</text></navLabel><content src=\"c{d}.xhtml\"/></navPoint>", .{ i, i });
        pos += written.len;
    }
    @memcpy(doc[pos .. pos + "</navMap></ncx>".len], "</navMap></ncx>");
    const doc_len = pos + "</navMap></ncx>".len;
    var book: epub_book_t = std.mem.zeroes(epub_book_t);
    try testing.expectEqual(k_ra8_err_no_mem, priv_epub_xml_parse_ncx(doc[0..doc_len].ptr, doc_len, &book));
    try testing.expectEqual(@as(u16, 0), book.toc_count);
}

test "parse_ncx guards its inputs" {
    var book: epub_book_t = std.mem.zeroes(epub_book_t);
    try testing.expectEqual(k_ra8_err_null_ptr, priv_epub_xml_parse_ncx(null, 4, &book));
    try testing.expectEqual(k_ra8_err_invalid_size, priv_epub_xml_parse_ncx("<?xml?>", 0, &book));
}

test "parse_nav prefers the typed nav and fills the entries" {
    const doc =
        "<?xml version=\"1.0\"?><html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\"><body>" ++
        "<nav epub:type=\"toc\"><ol><li><a href=\"c1.xhtml\">Chapter 1</a></li><li><a href=\"c2.xhtml\">Chapter 2</a></li></ol></nav>" ++
        "<nav><ol><li><a href=\"x1.xhtml\">Other 1</a></li></ol></nav>" ++
        "</body></html>";
    var book: epub_book_t = std.mem.zeroes(epub_book_t);
    try testing.expectEqual(k_ra8_ok, priv_epub_xml_parse_nav(doc, doc.len, &book));
    try testing.expectEqual(@as(u16, 2), book.toc_count);
    try expect_toc_entry(&book, 0, "Chapter 1", "c1.xhtml");
    try expect_toc_entry(&book, 1, "Chapter 2", "c2.xhtml");
}

test "parse_nav falls back to the first nav and takes span labels" {
    const doc =
        "<?xml version=\"1.0\"?><html xmlns=\"http://www.w3.org/1999/xhtml\"><body>" ++
        "<nav><ol><li><span>Top</span></li><li><a href=\"c2.xhtml\">Chapter 2</a></li></ol></nav>" ++
        "</body></html>";
    var book: epub_book_t = std.mem.zeroes(epub_book_t);
    try testing.expectEqual(k_ra8_ok, priv_epub_xml_parse_nav(doc, doc.len, &book));
    try testing.expectEqual(@as(u16, 2), book.toc_count);
    try expect_toc_entry(&book, 0, "Top", "");
    try expect_toc_entry(&book, 1, "Chapter 2", "c2.xhtml");
}

test "parse_nav refuses a nav without a direct ordered list" {
    const doc =
        "<?xml version=\"1.0\"?><html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\"><body>" ++
        "<nav epub:type=\"toc\"><p><a href=\"c1.xhtml\">Chapter 1</a></p></nav></body></html>";
    var book: epub_book_t = std.mem.zeroes(epub_book_t);
    try testing.expectEqual(k_ra8_err_validation_failed, priv_epub_xml_parse_nav(doc, doc.len, &book));
    try testing.expectEqual(@as(u16, 0), book.toc_count);
}

test "parse_nav guards its inputs" {
    var book: epub_book_t = std.mem.zeroes(epub_book_t);
    try testing.expectEqual(k_ra8_err_null_ptr, priv_epub_xml_parse_nav(null, 4, &book));
    try testing.expectEqual(k_ra8_err_invalid_size, priv_epub_xml_parse_nav("<?xml?>", 0, &book));
}
