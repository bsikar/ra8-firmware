//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for `epub_xml_shim_abi.zig`.

const std = @import("std");
const testing = std.testing;
const sut = @import("epub_xml_shim_abi");

const epub_book_t = sut.epub_book_t;
const epub_container_result_t = sut.epub_container_result_t;
const epub_xml_workspace_t = sut.epub_xml_workspace_t;
const k_epub_max_chapters = sut.k_epub_max_chapters;
const k_epub_toc_nav = sut.k_epub_toc_nav;
const k_epub_toc_ncx = sut.k_epub_toc_ncx;
const k_priv_mark_title = sut.k_priv_mark_title;
const k_ra8_err_invalid_size = sut.k_ra8_err_invalid_size;
const k_ra8_err_no_mem = sut.k_ra8_err_no_mem;
const k_ra8_err_null_ptr = sut.k_ra8_err_null_ptr;
const k_ra8_err_validation_failed = sut.k_ra8_err_validation_failed;
const k_ra8_ok = sut.k_ra8_ok;
const k_xml_event_none = sut.k_xml_event_none;
const k_xml_event_start = sut.k_xml_event_start;
const priv_epub_xml_ancestor_frame = sut.priv_epub_xml_ancestor_frame;
const priv_epub_xml_ancestor_marker = sut.priv_epub_xml_ancestor_marker;
const priv_epub_xml_attr = sut.priv_epub_xml_attr;
const priv_epub_xml_attr_contains = sut.priv_epub_xml_attr_contains;
const priv_epub_xml_copy = sut.priv_epub_xml_copy;
const priv_epub_xml_direct_child = sut.priv_epub_xml_direct_child;
const priv_epub_xml_find = sut.priv_epub_xml_find;
const priv_epub_xml_parse_container = sut.priv_epub_xml_parse_container;
const priv_epub_xml_parse_opf = sut.priv_epub_xml_parse_opf;
const priv_epub_xml_reader = sut.priv_epub_xml_reader;
const xml_event_t = sut.xml_event_t;
const xml_reader_next = sut.xml_reader_next;
const xml_reader_t = sut.xml_reader_t;
const xml_span_local_equal = sut.xml_span_local_equal;
const xml_span_t = sut.xml_span_t;

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
