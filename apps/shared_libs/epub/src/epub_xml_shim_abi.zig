//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Zig implementation of the strict bounded container/OPF XML consumers
//! (RA8FW-1164): the C-era src/epub_xml_shim.c ported verbatim behind the
//! unchanged C headers (epub_xml_shim_internal.h,
//! epub_xml_consumer_internal.h, epub_xml_opf_internal.h). The TU replays
//! validated immutable XML through a caller-owned pull-reader workspace and
//! commits EPUB metadata, spine, and TOC fields only after every syntax,
//! shape, and fixed-capacity preflight succeeds. The xml pull reader and
//! decoder (the ra8_xml archive) stay extern through the archive link, so
//! this TU adds no parser of its own. The remaining C consumer
//! epub_xml_toc.c reaches the shared helpers and the two parse entry points
//! through the archive link, so the C callers and the C++ tests stay
//! untouched. Ships as its own archive (ra8_epub_xml) so a consumer that
//! never parses container/OPF XML never pulls it; the archive sits after
//! ra8_epub_open in the link order.

const std = @import("std");

// ---- ra8_err_t (libs/ra8_core/inc/ra8_err.h, enum : uint16_t) --------------
const ra8_err_t = u16;
const k_ra8_ok: ra8_err_t = 0;
const k_ra8_err_no_mem: ra8_err_t = 0x102;
const k_ra8_err_invalid_size: ra8_err_t = 0x105;
const k_ra8_err_no_data: ra8_err_t = 0x10A;
const k_ra8_err_validation_failed: ra8_err_t = 0x501;
const k_ra8_err_null_ptr: ra8_err_t = 0x504;

// ---- epub.h limits ---------------------------------------------------------
const k_epub_max_chapters = 64;
const k_epub_max_toc = 64;
const k_epub_max_path_len = 192;
const k_epub_meta_len = 128;
const k_epub_max_fonts = 8;
const k_epub_max_manifest = 96;
const k_epub_id_len = 64;
const k_epub_media_len = 48;
const k_epub_zip_archive_bytes = 256;
const k_epub_toc_none: u8 = 0;
const k_epub_toc_ncx: u8 = 1;
const k_epub_toc_nav: u8 = 2;

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
const xml_attribute_t = extern struct { name: xml_span_t, value: xml_span_t };
const xml_attr_cursor_t = extern struct { position: u32, emitted: u16 };
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
extern fn xml_reader_init(reader: ?*xml_reader_t, source: ?[*]const u8, source_len: usize, workspace: ?*xml_workspace_t) callconv(.c) ra8_err_t;
extern fn xml_reader_next(reader: ?*xml_reader_t, out_event: ?*xml_event_t) callconv(.c) ra8_err_t;
extern fn xml_decode(source: ?[*]const u8, source_len: usize, span: xml_span_t, destination: ?[*]u8, capacity: usize, out_length: ?*usize) callconv(.c) ra8_err_t;
extern fn xml_decode_prefix(source: ?[*]const u8, source_len: usize, span: xml_span_t, destination: ?[*]u8, capacity: usize, out_length: ?*usize) callconv(.c) ra8_err_t;
extern fn xml_span_equal(source: ?[*]const u8, source_len: usize, span: xml_span_t, literal: [*:0]const u8) callconv(.c) bool;
extern fn xml_span_local_equal(source: ?[*]const u8, source_len: usize, span: xml_span_t, literal: [*:0]const u8) callconv(.c) bool;
extern fn xml_decoded_equal(source: ?[*]const u8, source_len: usize, left: xml_span_t, right: xml_span_t) callconv(.c) bool;
extern fn xml_attr_begin(event: ?*const xml_event_t, cursor: ?*xml_attr_cursor_t) callconv(.c) void;
extern fn xml_attr_next(source: ?[*]const u8, source_len: usize, event: ?*const xml_event_t, cursor: ?*xml_attr_cursor_t, out_attribute: ?*xml_attribute_t, out_has_value: ?*bool) callconv(.c) ra8_err_t;

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
pub const epub_book_t = extern struct {
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

/// Consumer markers stored in live reader frames (epub_xml_shim.c).
const k_priv_mark_none: u16 = 0;
const k_priv_mark_title: u16 = 1;
const k_priv_mark_creator: u16 = 2;
const k_priv_mark_language: u16 = 3;
const k_priv_mark_identifier: u16 = 4;
const k_priv_mark_identifier_match: u16 = 5;

/// Copy decoded text into a bounded EPUB field (truncates only at a complete
/// entity or codepoint).
pub export fn priv_epub_xml_copy(source: ?[*]const u8, source_len: usize, span: xml_span_t, destination: ?[*]u8, capacity: usize) callconv(.c) void {
    var ignored: usize = 0;
    _ = xml_decode_prefix(source, source_len, span, destination, capacity, &ignored);
}

/// Find an exact-name attribute on one start event, in source order.
pub export fn priv_epub_xml_attr(source: ?[*]const u8, source_len: usize, event: ?*const xml_event_t, name: [*:0]const u8) callconv(.c) priv_attr_t {
    var cursor: xml_attr_cursor_t = undefined;
    xml_attr_begin(event, &cursor);
    const event_ref = event.?;
    var i: u16 = 0;
    while (i < event_ref.attribute_count) : (i += 1) {
        var attribute: xml_attribute_t = undefined;
        var present: bool = false;
        if ((xml_attr_next(source, source_len, event, &cursor, &attribute, &present) != k_ra8_ok) or !present) {
            break;
        }
        if (xml_span_equal(source, source_len, attribute.name, name)) {
            return .{ .span = attribute.value, .present = true };
        }
    }
    return .{ .span = .{ .offset = 0, .length = 0 }, .present = false };
}

/// True when the decoded attribute value contains the needle substring.
pub export fn priv_epub_xml_attr_contains(source: ?[*]const u8, source_len: usize, attribute: priv_attr_t, needle: [*:0]const u8) callconv(.c) bool {
    if (!attribute.present) {
        return false;
    }
    var value: [k_epub_max_path_len]u8 = @splat(0);
    var length: usize = 0;
    if (xml_decode(source, source_len, attribute.span, &value, value.len, &length) != k_ra8_ok) {
        return false;
    }
    const decoded = std.mem.sliceTo(@as([*:0]const u8, @ptrCast(&value)), 0);
    const wanted = std.mem.span(needle);
    return std.mem.indexOf(u8, decoded, wanted) != null;
}

/// The element-name span retained on one live reader frame.
pub export fn priv_epub_xml_frame_name(reader: ?*const xml_reader_t, frame: u16) callconv(.c) xml_span_t {
    const value = &reader.?.workspace.?.frames[frame];
    return .{ .offset = value.name_offset, .length = value.name_length };
}

/// True when `child` is exactly one level under `parent`.
pub export fn priv_epub_xml_direct_child(child: u16, parent: u16) callconv(.c) bool {
    if (parent == 0xFFFF) {
        return false;
    }
    return child == (parent +% 1);
}

/// Walk `levels` frames up from `depth`; false when the walk falls off the root.
pub export fn priv_epub_xml_ancestor_frame(depth: u16, levels: u16, out_frame: ?*u16) callconv(.c) bool {
    var frame: u16 = depth;
    var remaining: u16 = levels;
    while (remaining > 0) {
        if (frame == 0) {
            return false;
        }
        frame -= 1;
        remaining -= 1;
    }
    out_frame.?.* = frame;
    return true;
}

/// The consumer marker carried by the frame `levels` above `depth`.
pub export fn priv_epub_xml_ancestor_marker(reader: ?*const xml_reader_t, depth: u16, levels: u16) callconv(.c) u16 {
    var frame: u16 = 0;
    return if (priv_epub_xml_ancestor_frame(depth, levels, &frame))
        reader.?.workspace.?.frames[frame].consumer
    else
        0;
}

/// Bind a pull pass to caller-owned stack storage.
pub export fn priv_epub_xml_reader(reader: ?*xml_reader_t, source: ?[*]const u8, length: usize, workspace: ?*epub_xml_workspace_t) callconv(.c) ra8_err_t {
    return xml_reader_init(reader, source, length, &workspace.?.reader);
}

/// Find the first start event of a local name, optionally under one parent.
pub export fn priv_epub_xml_find(source: ?[*]const u8, length: usize, workspace: ?*epub_xml_workspace_t, local: [*:0]const u8, use_parent: bool, parent_offset: u32, parent_depth: u16, out: ?*xml_event_t) callconv(.c) ra8_err_t {
    var reader: xml_reader_t = undefined;
    var err: ra8_err_t = priv_epub_xml_reader(&reader, source, length, workspace);
    var parent_active: bool = false;
    while (err == k_ra8_ok) {
        var event: xml_event_t = undefined;
        err = xml_reader_next(&reader, &event);
        if ((err != k_ra8_ok) or (event.kind == k_xml_event_none)) {
            break;
        }
        if (use_parent and (event.kind == k_xml_event_start) and (event.depth == parent_depth) and
            (event.markup.offset == parent_offset))
        {
            parent_active = true;
            continue;
        }
        if ((event.kind == k_xml_event_start) and
            xml_span_local_equal(source, length, event.name, local))
        {
            if (!use_parent or (parent_active and priv_epub_xml_direct_child(event.depth, parent_depth))) {
                out.?.* = event;
                return k_ra8_ok;
            }
        } else if (use_parent and parent_active and (event.kind == k_xml_event_end) and
            (event.depth == parent_depth))
        {
            return k_ra8_err_validation_failed;
        }
    }
    return if (err == k_ra8_ok) k_ra8_err_validation_failed else err;
}

/// Parse META-INF/container.xml and copy the rootfile path out.
pub export fn priv_epub_xml_parse_container(xml_bytes: ?[*]const u8, xml_len: usize, out: ?*epub_container_result_t, workspace: ?*epub_xml_workspace_t) callconv(.c) ra8_err_t {
    if ((xml_bytes == null) or (out == null) or (workspace == null)) {
        return k_ra8_err_null_ptr;
    }
    if (xml_len == 0) {
        return k_ra8_err_invalid_size;
    }
    var err: ra8_err_t = xml_validate(xml_bytes, xml_len, &workspace.?.reader);
    var rootfiles: xml_event_t = undefined;
    if (err == k_ra8_ok) {
        err = priv_epub_xml_find(xml_bytes, xml_len, workspace, "rootfiles", false, 0, 0, &rootfiles);
    }
    var rootfile: xml_event_t = undefined;
    if (err == k_ra8_ok) {
        err = priv_epub_xml_find(xml_bytes, xml_len, workspace, "rootfile", true, rootfiles.markup.offset, rootfiles.depth, &rootfile);
    } else if (err == k_ra8_err_validation_failed) {
        err = priv_epub_xml_find(xml_bytes, xml_len, workspace, "rootfile", false, 0, 0, &rootfile);
    }
    if (err != k_ra8_ok) {
        return err;
    }
    const path = priv_epub_xml_attr(xml_bytes, xml_len, &rootfile, "full-path");
    if (!path.present or (path.span.length == 0)) {
        return k_ra8_err_validation_failed;
    }
    out.?.opf_path[0] = 0;
    priv_epub_xml_copy(xml_bytes, xml_len, path.span, &out.?.opf_path, out.?.opf_path.len);
    return k_ra8_ok;
}

/// True when the media-type names an embedded font.
fn internal_font_type(source: [*]const u8, source_len: usize, media: priv_attr_t) bool {
    const types = [_][*:0]const u8{
        "application/font-sfnt",
        "application/vnd.ms-opentype",
        "font/ttf",
        "font/otf",
        "application/x-font-ttf",
    };
    for (types) |kind| {
        if (media.present and xml_span_equal(source, source_len, media.span, kind)) {
            return true;
        }
    }
    return false;
}

/// Record one manifest item, its embedded-font and cover/nav properties.
fn internal_manifest_item(source: [*]const u8, source_len: usize, event: *const xml_event_t, book: *epub_book_t) void {
    const id = priv_epub_xml_attr(source, source_len, event, "id");
    const href = priv_epub_xml_attr(source, source_len, event, "href");
    const media = priv_epub_xml_attr(source, source_len, event, "media-type");
    const properties = priv_epub_xml_attr(source, source_len, event, "properties");
    if (book.manifest_count < k_epub_max_manifest) {
        const item = &book.manifest[book.manifest_count];
        book.manifest_count += 1;
        if (id.present) {
            priv_epub_xml_copy(source, source_len, id.span, &item.id, item.id.len);
        }
        if (href.present) {
            priv_epub_xml_copy(source, source_len, href.span, &item.href, item.href.len);
        }
        if (media.present) {
            priv_epub_xml_copy(source, source_len, media.span, &item.media_type, item.media_type.len);
        }
    }
    if (href.present and internal_font_type(source, source_len, media) and
        (book.embedded_font_count < k_epub_max_fonts))
    {
        priv_epub_xml_copy(source, source_len, href.span, &book.embedded_font_paths[book.embedded_font_count], k_epub_max_path_len);
        book.embedded_font_count += 1;
    }
    if (href.present and priv_epub_xml_attr_contains(source, source_len, properties, "cover-image")) {
        priv_epub_xml_copy(source, source_len, href.span, &book.cover_path, book.cover_path.len);
    }
    if (href.present and priv_epub_xml_attr_contains(source, source_len, properties, "nav")) {
        priv_epub_xml_copy(source, source_len, href.span, &book.toc_path, book.toc_path.len);
        book.toc_kind = k_epub_toc_nav;
    }
}

/// Commit a text run under a marked Dublin Core element.
fn internal_metadata_text(source: [*]const u8, source_len: usize, event: *const xml_event_t, reader: *const xml_reader_t, book: *epub_book_t) void {
    if (event.depth == 0) {
        return;
    }
    const marker = priv_epub_xml_ancestor_marker(reader, event.depth, 1);
    if (marker == k_priv_mark_title) {
        priv_epub_xml_copy(source, source_len, event.markup, &book.title, book.title.len);
    } else if (marker == k_priv_mark_creator) {
        priv_epub_xml_copy(source, source_len, event.markup, &book.author, book.author.len);
    } else if (marker == k_priv_mark_language) {
        priv_epub_xml_copy(source, source_len, event.markup, &book.language, book.language.len);
    } else if ((marker == k_priv_mark_identifier_match) or
        ((marker == k_priv_mark_identifier) and (book.identifier[0] == 0)))
    {
        priv_epub_xml_copy(source, source_len, event.markup, &book.identifier, book.identifier.len);
    }
}

/// Mark one direct metadata child in the reader frames.
fn internal_mark_metadata(source: [*]const u8, source_len: usize, event: *const xml_event_t, reader: *xml_reader_t, unique_id: xml_span_t) void {
    var marker: u16 = k_priv_mark_none;
    if (xml_span_local_equal(source, source_len, event.name, "title")) {
        marker = k_priv_mark_title;
    } else if (xml_span_local_equal(source, source_len, event.name, "creator")) {
        marker = k_priv_mark_creator;
    } else if (xml_span_local_equal(source, source_len, event.name, "language")) {
        marker = k_priv_mark_language;
    } else if (xml_span_local_equal(source, source_len, event.name, "identifier")) {
        marker = k_priv_mark_identifier;
        const id = priv_epub_xml_attr(source, source_len, event, "id");
        if (id.present and (unique_id.length > 0) and
            xml_decoded_equal(source, source_len, id.span, unique_id))
        {
            marker = k_priv_mark_identifier_match;
        }
    }
    reader.workspace.?.frames[event.depth].consumer = marker;
}

/// Fold the shape gate: missing manifest or spine is a validation failure.
fn internal_opf_status(err: ra8_err_t, manifest_depth: u16, spine_depth: u16) ra8_err_t {
    return if ((err == k_ra8_ok) and ((manifest_depth == 0xFFFF) or (spine_depth == 0xFFFF)))
        k_ra8_err_validation_failed
    else
        err;
}

/// Record one direct metadata child of the OPF metadata element.
fn internal_opf_metadata_child(source: [*]const u8, length: usize, event: *const xml_event_t, reader: *xml_reader_t, unique_id: xml_span_t, book: *epub_book_t) void {
    internal_mark_metadata(source, length, event, reader, unique_id);
    if (!xml_span_local_equal(source, length, event.name, "meta")) {
        return;
    }
    const name = priv_epub_xml_attr(source, length, event, "name");
    const content = priv_epub_xml_attr(source, length, event, "content");
    if (name.present and content.present and xml_span_equal(source, length, name.span, "cover")) {
        book.xml_workspace.legacy_cover_id = content.span;
    }
}

/// Classify one OPF start-tag event during the first parse pass.
fn internal_opf_first_event(source: [*]const u8, length: usize, event: *const xml_event_t, reader: *xml_reader_t, book: *epub_book_t, metadata_depth: *u16, manifest_depth: *u16, spine_depth: *u16, unique_id: *xml_span_t, out_spine_toc: *xml_span_t) void {
    if (event.depth == 0) {
        const uid = priv_epub_xml_attr(source, length, event, "unique-identifier");
        unique_id.* = if (uid.present) uid.span else .{ .offset = 0, .length = 0 };
    } else if ((event.depth == 1) and
        xml_span_local_equal(source, length, event.name, "metadata"))
    {
        metadata_depth.* = event.depth;
    } else if ((event.depth == 1) and
        xml_span_local_equal(source, length, event.name, "manifest"))
    {
        manifest_depth.* = event.depth;
    } else if ((event.depth == 1) and xml_span_local_equal(source, length, event.name, "spine")) {
        spine_depth.* = event.depth;
        const toc = priv_epub_xml_attr(source, length, event, "toc");
        out_spine_toc.* = if (toc.present) toc.span else .{ .offset = 0, .length = 0 };
    } else if (priv_epub_xml_direct_child(event.depth, metadata_depth.*) and
        xml_span_local_equal(source, length, priv_epub_xml_frame_name(reader, metadata_depth.*), "metadata"))
    {
        internal_opf_metadata_child(source, length, event, reader, unique_id.*, book);
    } else if (priv_epub_xml_direct_child(event.depth, manifest_depth.*) and
        xml_span_local_equal(source, length, priv_epub_xml_frame_name(reader, manifest_depth.*), "manifest") and
        xml_span_local_equal(source, length, event.name, "item"))
    {
        internal_manifest_item(source, length, event, book);
    }
}

/// First parse pass: metadata marks, manifest records, and the spine toc span.
fn internal_opf_first(source: [*]const u8, length: usize, book: *epub_book_t, out_spine_toc: *xml_span_t) ra8_err_t {
    var reader: xml_reader_t = undefined;
    var err: ra8_err_t = priv_epub_xml_reader(&reader, source, length, &book.xml_workspace);
    var metadata_depth: u16 = 0xFFFF;
    var manifest_depth: u16 = 0xFFFF;
    var spine_depth: u16 = 0xFFFF;
    var unique_id: xml_span_t = .{ .offset = 0, .length = 0 };
    while (err == k_ra8_ok) {
        var event: xml_event_t = undefined;
        err = xml_reader_next(&reader, &event);
        if ((err != k_ra8_ok) or (event.kind == k_xml_event_none)) {
            break;
        }
        if (event.kind == k_xml_event_text) {
            internal_metadata_text(source, length, &event, &reader, book);
            continue;
        }
        if (event.kind != k_xml_event_start) {
            continue;
        }
        internal_opf_first_event(source, length, &event, &reader, book, &metadata_depth, &manifest_depth, &spine_depth, &unique_id, out_spine_toc);
    }
    return internal_opf_status(err, manifest_depth, spine_depth);
}

/// Collect the spine itemref spans into the workspace reference table.
fn internal_collect_spine(source: [*]const u8, length: usize, book: *epub_book_t) ra8_err_t {
    var reader: xml_reader_t = undefined;
    var err: ra8_err_t = priv_epub_xml_reader(&reader, source, length, &book.xml_workspace);
    var spine_depth: u16 = 0xFFFF;
    book.xml_workspace.reference_count = 0;
    while (err == k_ra8_ok) {
        var event: xml_event_t = undefined;
        err = xml_reader_next(&reader, &event);
        if ((err != k_ra8_ok) or (event.kind == k_xml_event_none)) {
            break;
        }
        if (event.kind != k_xml_event_start) {
            continue;
        }
        if ((event.depth == 1) and xml_span_local_equal(source, length, event.name, "spine")) {
            spine_depth = event.depth;
        } else if (priv_epub_xml_direct_child(event.depth, spine_depth) and
            xml_span_local_equal(source, length, priv_epub_xml_frame_name(&reader, spine_depth), "spine") and
            xml_span_equal(source, length, event.name, "itemref"))
        {
            const idref = priv_epub_xml_attr(source, length, &event, "idref");
            if (!idref.present) {
                continue;
            }
            if (book.xml_workspace.reference_count >= k_epub_max_chapters) {
                return k_ra8_err_no_mem;
            }
            book.xml_workspace.references[book.xml_workspace.reference_count] = idref.span;
            book.xml_workspace.reference_count += 1;
        }
    }
    return err;
}

/// Look one manifest id up against the OPF and return its href span.
fn internal_manifest_lookup(source: [*]const u8, length: usize, workspace: *epub_xml_workspace_t, wanted: xml_span_t, out_href: *xml_span_t) ra8_err_t {
    var reader: xml_reader_t = undefined;
    var err: ra8_err_t = priv_epub_xml_reader(&reader, source, length, workspace);
    var manifest_depth: u16 = 0xFFFF;
    while (err == k_ra8_ok) {
        var event: xml_event_t = undefined;
        err = xml_reader_next(&reader, &event);
        if ((err != k_ra8_ok) or (event.kind == k_xml_event_none)) {
            break;
        }
        if (event.kind != k_xml_event_start) {
            continue;
        }
        if ((event.depth == 1) and xml_span_local_equal(source, length, event.name, "manifest")) {
            manifest_depth = event.depth;
        } else if (priv_epub_xml_direct_child(event.depth, manifest_depth) and
            xml_span_local_equal(source, length, priv_epub_xml_frame_name(&reader, manifest_depth), "manifest"))
        {
            const id = priv_epub_xml_attr(source, length, &event, "id");
            const href = priv_epub_xml_attr(source, length, &event, "href");
            if (id.present and href.present and xml_decoded_equal(source, length, id.span, wanted)) {
                out_href.* = href.span;
                return k_ra8_ok;
            }
        }
    }
    return if (err == k_ra8_ok) k_ra8_err_no_data else err;
}

/// Validate OPF structure and spine capacity before semantic mutation.
fn internal_opf_shape(source: [*]const u8, length: usize, workspace: *epub_xml_workspace_t) ra8_err_t {
    var reader: xml_reader_t = undefined;
    var err: ra8_err_t = priv_epub_xml_reader(&reader, source, length, workspace);
    var saw_manifest: bool = false;
    var saw_spine: bool = false;
    var spine_depth: u16 = 0xFFFF;
    var references: u16 = 0;
    while (err == k_ra8_ok) {
        var event: xml_event_t = undefined;
        err = xml_reader_next(&reader, &event);
        if ((err != k_ra8_ok) or (event.kind == k_xml_event_none)) {
            break;
        }
        if (event.kind != k_xml_event_start) {
            continue;
        }
        if ((event.depth == 1) and xml_span_local_equal(source, length, event.name, "manifest")) {
            saw_manifest = true;
        } else if ((event.depth == 1) and xml_span_local_equal(source, length, event.name, "spine")) {
            saw_spine = true;
            spine_depth = event.depth;
        } else if (priv_epub_xml_direct_child(event.depth, spine_depth) and
            xml_span_equal(source, length, event.name, "itemref") and
            priv_epub_xml_attr(source, length, &event, "idref").present)
        {
            if (references >= k_epub_max_chapters) {
                return k_ra8_err_no_mem;
            }
            references += 1;
        }
    }
    if ((err == k_ra8_ok) and (!saw_manifest or !saw_spine)) {
        return k_ra8_err_validation_failed;
    }
    return err;
}

/// Resolve chapter, cover, and TOC hrefs from cross-referenced ids.
fn internal_opf_resolve_refs(xml_bytes: [*]const u8, xml_len: usize, spine_toc: xml_span_t, book: *epub_book_t) ra8_err_t {
    var err: ra8_err_t = k_ra8_ok;
    book.chapter_count = 0;
    var i: u16 = 0;
    while ((err == k_ra8_ok) and (i < book.xml_workspace.reference_count)) : (i += 1) {
        var href: xml_span_t = .{ .offset = 0, .length = 0 };
        const found = internal_manifest_lookup(xml_bytes, xml_len, &book.xml_workspace, book.xml_workspace.references[i], &href);
        if (found == k_ra8_ok) {
            const chapter = book.chapter_count;
            priv_epub_xml_copy(xml_bytes, xml_len, href, &book.chapter_paths[chapter], k_epub_max_path_len);
            book.chapter_count += 1;
        } else if (found != k_ra8_err_no_data) {
            err = found;
        }
    }
    if ((err == k_ra8_ok) and (book.cover_path[0] == 0) and
        (book.xml_workspace.legacy_cover_id.length > 0))
    {
        var href: xml_span_t = .{ .offset = 0, .length = 0 };
        if (internal_manifest_lookup(xml_bytes, xml_len, &book.xml_workspace, book.xml_workspace.legacy_cover_id, &href) == k_ra8_ok) {
            priv_epub_xml_copy(xml_bytes, xml_len, href, &book.cover_path, book.cover_path.len);
        }
    }
    if ((err == k_ra8_ok) and (book.toc_kind != k_epub_toc_nav) and (spine_toc.length > 0)) {
        var href: xml_span_t = .{ .offset = 0, .length = 0 };
        if (internal_manifest_lookup(xml_bytes, xml_len, &book.xml_workspace, spine_toc, &href) == k_ra8_ok) {
            priv_epub_xml_copy(xml_bytes, xml_len, href, &book.toc_path, book.toc_path.len);
            book.toc_kind = k_epub_toc_ncx;
        }
    }
    return err;
}

/// Parse the OPF document into metadata, cover and spine.
pub export fn priv_epub_xml_parse_opf(xml_bytes: ?[*]const u8, xml_len: usize, book: ?*epub_book_t) callconv(.c) ra8_err_t {
    if ((xml_bytes == null) or (book == null)) {
        return k_ra8_err_null_ptr;
    }
    if (xml_len == 0) {
        return k_ra8_err_invalid_size;
    }
    var err: ra8_err_t = xml_validate(xml_bytes, xml_len, &book.?.xml_workspace.reader);
    var spine_toc: xml_span_t = .{ .offset = 0, .length = 0 };
    if (err == k_ra8_ok) {
        err = internal_opf_shape(xml_bytes.?, xml_len, &book.?.xml_workspace);
    }
    if (err == k_ra8_ok) {
        err = internal_opf_first(xml_bytes.?, xml_len, book.?, &spine_toc);
    }
    if (err == k_ra8_ok) {
        err = internal_collect_spine(xml_bytes.?, xml_len, book.?);
    }
    if (err == k_ra8_ok) {
        err = internal_opf_resolve_refs(xml_bytes.?, xml_len, spine_toc, book.?);
    }
    return err;
}

const test_container_xml =
    "<?xml version=\"1.0\"?>\n" ++
    "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n" ++
    "  <rootfiles>\n" ++
    "    <rootfile full-path=\"OEBPS/content.opf\" media-type=\"application/oebps-package+xml\"/>\n" ++
    "  </rootfiles>\n" ++
    "</container>";

const test_opf_xml =
    "<?xml version=\"1.0\"?>\n" ++
    "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" unique-identifier=\"pub-id\">\n" ++
    "  <metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n" ++
    "    <dc:title>Test Book</dc:title>\n" ++
    "    <dc:creator>Author Name</dc:creator>\n" ++
    "    <dc:language>en</dc:language>\n" ++
    "    <dc:identifier id=\"pub-id\">urn:uuid:1234</dc:identifier>\n" ++
    "    <meta name=\"cover\" content=\"cover-image\"/>\n" ++
    "  </metadata>\n" ++
    "  <manifest>\n" ++
    "    <item id=\"titlepage\" href=\"title.xhtml\" media-type=\"application/xhtml+xml\"/>\n" ++
    "    <item id=\"cover-image\" href=\"cover.jpg\" media-type=\"image/jpeg\" properties=\"cover-image\"/>\n" ++
    "    <item id=\"nav\" href=\"nav.xhtml\" media-type=\"application/xhtml+xml\" properties=\"nav\"/>\n" ++
    "    <item id=\"font1\" href=\"font.ttf\" media-type=\"font/ttf\"/>\n" ++
    "  </manifest>\n" ++
    "  <spine toc=\"nav\">\n" ++
    "    <itemref idref=\"titlepage\"/>\n" ++
    "  </spine>\n" ++
    "</package>";

fn test_book() epub_book_t {
    return std.mem.zeroes(epub_book_t);
}

fn span_of(text: []const u8) xml_span_t {
    return .{ .offset = 0, .length = @intCast(text.len) };
}

test "copy decodes entities and truncates only at a complete prefix" {
    const source = "&lt;T&amp;itle&gt;suffix";
    var out: [8]u8 = @splat(0);
    priv_epub_xml_copy(source, source.len, span_of(source), &out, out.len);
    try std.testing.expectEqualStrings("<T&itle", std.mem.sliceTo(@as([*:0]const u8, @ptrCast(&out)), 0));
}

test "attr finds the wanted attribute and misses the absent one" {
    const source = "<rootfile full-path=\"a.opf\" media-type=\"x\"/>";
    var event: xml_event_t = undefined;
    var workspace: epub_xml_workspace_t = std.mem.zeroes(epub_xml_workspace_t);
    try std.testing.expectEqual(k_ra8_ok, priv_epub_xml_find(source, source.len, &workspace, "rootfile", false, 0, 0, &event));
    const path = priv_epub_xml_attr(source, source.len, &event, "full-path");
    try std.testing.expect(path.present);
    const missing = priv_epub_xml_attr(source, source.len, &event, "href");
    try std.testing.expect(!missing.present);
}

test "attr_contains needs a present attribute and a matching needle" {
    const source = "<item properties=\"cover-image nav\"/>";
    var event: xml_event_t = undefined;
    var workspace: epub_xml_workspace_t = std.mem.zeroes(epub_xml_workspace_t);
    try std.testing.expectEqual(k_ra8_ok, priv_epub_xml_find(source, source.len, &workspace, "item", false, 0, 0, &event));
    const properties = priv_epub_xml_attr(source, source.len, &event, "properties");
    try std.testing.expect(priv_epub_xml_attr_contains(source, source.len, properties, "cover-image"));
    try std.testing.expect(!priv_epub_xml_attr_contains(source, source.len, properties, "epub"));
    try std.testing.expect(!priv_epub_xml_attr_contains(source, source.len, .{ .span = .{ .offset = 0, .length = 0 }, .present = false }, "nav"));
}

test "direct_child rejects the unparented sentinel and walks one level" {
    try std.testing.expect(!priv_epub_xml_direct_child(1, 0xFFFF));
    try std.testing.expect(priv_epub_xml_direct_child(3, 2));
    try std.testing.expect(!priv_epub_xml_direct_child(4, 2));
}

test "ancestor_frame walks up and fails past the root" {
    var frame: u16 = 0;
    try std.testing.expect(priv_epub_xml_ancestor_frame(3, 2, &frame));
    try std.testing.expectEqual(@as(u16, 1), frame);
    try std.testing.expect(!priv_epub_xml_ancestor_frame(1, 2, &frame));
}

test "ancestor_marker reads the marked frame above the depth" {
    const source = "<metadata><title>Text</title></metadata>";
    var workspace: epub_xml_workspace_t = std.mem.zeroes(epub_xml_workspace_t);
    var reader: xml_reader_t = undefined;
    try std.testing.expectEqual(k_ra8_ok, priv_epub_xml_reader(&reader, source, source.len, &workspace));
    var event: xml_event_t = undefined;
    var title_depth: u16 = 0;
    while (true) {
        try std.testing.expectEqual(k_ra8_ok, xml_reader_next(&reader, &event));
        if (event.kind == k_xml_event_none) break;
        if ((event.kind == k_xml_event_start) and xml_span_local_equal(source, source.len, event.name, "title")) {
            title_depth = event.depth;
            workspace.reader.frames[event.depth].consumer = k_priv_mark_title;
        }
    }
    try std.testing.expectEqual(k_priv_mark_title, priv_epub_xml_ancestor_marker(&reader, title_depth + 1, 1));
    try std.testing.expectEqual(@as(u16, 0), priv_epub_xml_ancestor_marker(&reader, 0, 1));
}

test "find locates a start event without a parent gate" {
    const source = test_container_xml;
    var workspace: epub_xml_workspace_t = std.mem.zeroes(epub_xml_workspace_t);
    var out: xml_event_t = undefined;
    try std.testing.expectEqual(k_ra8_ok, priv_epub_xml_find(source, source.len, &workspace, "rootfiles", false, 0, 0, &out));
    try std.testing.expectEqual(@as(u16, 1), out.depth);
}

test "find without a parent returns validation_failed when absent" {
    const source = "<container><rootfiles/></container>";
    var workspace: epub_xml_workspace_t = std.mem.zeroes(epub_xml_workspace_t);
    var out: xml_event_t = undefined;
    try std.testing.expectEqual(k_ra8_err_validation_failed, priv_epub_xml_find(source, source.len, &workspace, "rootfile", false, 0, 0, &out));
}

test "find with a parent only accepts direct children of that parent" {
    const source = "<a><parent><child/></parent></a>";
    var workspace: epub_xml_workspace_t = std.mem.zeroes(epub_xml_workspace_t);
    var parent: xml_event_t = undefined;
    try std.testing.expectEqual(k_ra8_ok, priv_epub_xml_find(source, source.len, &workspace, "parent", false, 0, 0, &parent));
    var out: xml_event_t = undefined;
    try std.testing.expectEqual(k_ra8_ok, priv_epub_xml_find(source, source.len, &workspace, "child", true, parent.markup.offset, parent.depth, &out));
    try std.testing.expectEqual(k_ra8_err_validation_failed, priv_epub_xml_find(source, source.len, &workspace, "parent", true, parent.markup.offset, parent.depth, &out));
}

test "parse_container rejects null, empty, and malformed documents" {
    var out: epub_container_result_t = std.mem.zeroes(epub_container_result_t);
    var workspace: epub_xml_workspace_t = std.mem.zeroes(epub_xml_workspace_t);
    try std.testing.expectEqual(k_ra8_err_null_ptr, priv_epub_xml_parse_container(null, 4, &out, &workspace));
    try std.testing.expectEqual(k_ra8_err_invalid_size, priv_epub_xml_parse_container("<a/>", 0, &out, &workspace));
    try std.testing.expectEqual(k_ra8_err_validation_failed, priv_epub_xml_parse_container("<a><b></c></a>", 13, &out, &workspace));
}

test "parse_container copies the first rootfile path" {
    var out: epub_container_result_t = std.mem.zeroes(epub_container_result_t);
    var workspace: epub_xml_workspace_t = std.mem.zeroes(epub_xml_workspace_t);
    try std.testing.expectEqual(k_ra8_ok, priv_epub_xml_parse_container(test_container_xml, test_container_xml.len, &out, &workspace));
    try std.testing.expectEqualStrings("OEBPS/content.opf", std.mem.sliceTo(@as([*:0]const u8, @ptrCast(&out.opf_path)), 0));
}

test "parse_container survives a missing rootfiles wrapper" {
    const source = "<container><rootfile full-path=\"x.opf\"/></container>";
    var out: epub_container_result_t = std.mem.zeroes(epub_container_result_t);
    var workspace: epub_xml_workspace_t = std.mem.zeroes(epub_xml_workspace_t);
    try std.testing.expectEqual(k_ra8_ok, priv_epub_xml_parse_container(source, source.len, &out, &workspace));
    try std.testing.expectEqualStrings("x.opf", std.mem.sliceTo(@as([*:0]const u8, @ptrCast(&out.opf_path)), 0));
}

test "parse_container rejects a rootfile without a full-path" {
    const source = "<container><rootfiles><rootfile/></rootfiles></container>";
    var out: epub_container_result_t = std.mem.zeroes(epub_container_result_t);
    var workspace: epub_xml_workspace_t = std.mem.zeroes(epub_xml_workspace_t);
    try std.testing.expectEqual(k_ra8_err_validation_failed, priv_epub_xml_parse_container(source, source.len, &out, &workspace));
}

test "parse_opf rejects null and empty inputs" {
    var book = test_book();
    try std.testing.expectEqual(k_ra8_err_null_ptr, priv_epub_xml_parse_opf(null, 4, &book));
    try std.testing.expectEqual(k_ra8_err_invalid_size, priv_epub_xml_parse_opf("<a/>", 0, &book));
}

test "parse_opf rejects a package without manifest or spine" {
    const source = "<package/>";
    var book = test_book();
    try std.testing.expectEqual(k_ra8_err_validation_failed, priv_epub_xml_parse_opf(source, source.len, &book));
}

test "parse_opf rejects a spine over the fixed chapter capacity" {
    var source: std.ArrayListUnmanaged(u8) = .empty;
    defer source.deinit(std.testing.allocator);
    try source.appendSlice(std.testing.allocator, "<package><manifest><item id=\"i0\" href=\"0.xhtml\"/></manifest><spine>");
    var i: usize = 0;
    while (i <= k_epub_max_chapters) : (i += 1) {
        var ref: [32]u8 = undefined;
        const written = try std.fmt.bufPrint(&ref, "<itemref idref=\"i{d}\"/>", .{i});
        try source.appendSlice(std.testing.allocator, written);
    }
    try source.appendSlice(std.testing.allocator, "</spine></package>");
    var book = test_book();
    try std.testing.expectEqual(k_ra8_err_no_mem, priv_epub_xml_parse_opf(source.items.ptr, source.items.len, &book));
}

test "parse_opf commits metadata, spine, cover, fonts and nav toc" {
    var book = test_book();
    try std.testing.expectEqual(k_ra8_ok, priv_epub_xml_parse_opf(test_opf_xml, test_opf_xml.len, &book));
    try std.testing.expectEqualStrings("Test Book", std.mem.sliceTo(@as([*:0]const u8, @ptrCast(&book.title)), 0));
    try std.testing.expectEqualStrings("Author Name", std.mem.sliceTo(@as([*:0]const u8, @ptrCast(&book.author)), 0));
    try std.testing.expectEqualStrings("en", std.mem.sliceTo(@as([*:0]const u8, @ptrCast(&book.language)), 0));
    try std.testing.expectEqualStrings("urn:uuid:1234", std.mem.sliceTo(@as([*:0]const u8, @ptrCast(&book.identifier)), 0));
    try std.testing.expectEqual(@as(u16, 4), book.manifest_count);
    try std.testing.expectEqualStrings("title.xhtml", std.mem.sliceTo(@as([*:0]const u8, @ptrCast(&book.manifest[0].href)), 0));
    try std.testing.expectEqual(@as(u16, 1), book.embedded_font_count);
    try std.testing.expectEqualStrings("font.ttf", std.mem.sliceTo(@as([*:0]const u8, @ptrCast(&book.embedded_font_paths[0])), 0));
    try std.testing.expectEqualStrings("cover.jpg", std.mem.sliceTo(@as([*:0]const u8, @ptrCast(&book.cover_path)), 0));
    try std.testing.expectEqualStrings("nav.xhtml", std.mem.sliceTo(@as([*:0]const u8, @ptrCast(&book.toc_path)), 0));
    try std.testing.expectEqual(k_epub_toc_nav, book.toc_kind);
    try std.testing.expectEqual(@as(u16, 1), book.chapter_count);
    try std.testing.expectEqualStrings("title.xhtml", std.mem.sliceTo(@as([*:0]const u8, @ptrCast(&book.chapter_paths[0])), 0));
}

test "parse_opf falls back to the spine toc attribute for the NCX path" {
    var source: std.ArrayListUnmanaged(u8) = .empty;
    defer source.deinit(std.testing.allocator);
    try source.appendSlice(std.testing.allocator, "<package unique-identifier=\"uid\"><metadata><identifier id=\"uid\">book-id</identifier></metadata><manifest><item id=\"ncx\" href=\"toc.ncx\"/></manifest><spine toc=\"ncx\"><itemref idref=\"ncx\"/></spine></package>");
    var book = test_book();
    try std.testing.expectEqual(k_ra8_ok, priv_epub_xml_parse_opf(source.items.ptr, source.items.len, &book));
    try std.testing.expectEqualStrings("book-id", std.mem.sliceTo(@as([*:0]const u8, @ptrCast(&book.identifier)), 0));
    try std.testing.expectEqualStrings("toc.ncx", std.mem.sliceTo(@as([*:0]const u8, @ptrCast(&book.toc_path)), 0));
    try std.testing.expectEqual(k_epub_toc_ncx, book.toc_kind);
}

test "parse_opf resolves the legacy EPUB 2 cover meta" {
    const source = "<package><metadata><meta name=\"cover\" content=\"c0\"/></metadata><manifest><item id=\"c0\" href=\"pic.png\" media-type=\"image/png\"/></manifest><spine/></package>";
    var book = test_book();
    try std.testing.expectEqual(k_ra8_ok, priv_epub_xml_parse_opf(source, source.len, &book));
    try std.testing.expectEqualStrings("pic.png", std.mem.sliceTo(@as([*:0]const u8, @ptrCast(&book.cover_path)), 0));
}
