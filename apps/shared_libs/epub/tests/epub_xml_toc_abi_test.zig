//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for `epub_xml_toc_abi.zig`.

const std = @import("std");
const testing = std.testing;
const sut = @import("epub_xml_toc_abi");

const epub_book_t = sut.epub_book_t;
const k_epub_max_toc = sut.k_epub_max_toc;
const k_ra8_err_invalid_size = sut.k_ra8_err_invalid_size;
const k_ra8_err_no_mem = sut.k_ra8_err_no_mem;
const k_ra8_err_null_ptr = sut.k_ra8_err_null_ptr;
const k_ra8_err_validation_failed = sut.k_ra8_err_validation_failed;
const k_ra8_ok = sut.k_ra8_ok;
const priv_epub_xml_parse_nav = sut.priv_epub_xml_parse_nav;
const priv_epub_xml_parse_ncx = sut.priv_epub_xml_parse_ncx;

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
