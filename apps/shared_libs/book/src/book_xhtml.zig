//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Resident XHTML serializer + plain-text extractor (RA8FW-1250). Ported 1:1
//! from apps/shared_libs/book/src/book_xhtml.c, deleted in the same commit.
//! `reflow_layout_chapter()` consumes XHTML; this bridge walks a chapter's
//! pre-parsed book DOM and re-emits well-formed XHTML so a compiled book
//! renders through the existing, MC/DC-tested reflow engine without
//! modifying it. The walk is iterative (an explicit open/close stack, NASA
//! Rule 1) and bounded (NASA Rule 2): nothing recurses, the stack and the
//! iteration count are capped. Serialization is lossless: every element tag,
//! attribute and text run is preserved; void elements self-close and text /
//! attribute values are entity-escaped.
//!
//! Six C ABI symbols stay exported (book_chapter_text,
//! book_chapter_to_xhtml, priv_book_is_block, priv_book_emit_text,
//! priv_book_emit_break) for the C test suites and every C caller. The
//! resident container API (book_open and friends) still lives in book.c,
//! which stays C; this TU reads a validated blob's tables directly through
//! the same inline accessors the C bridge used (book_header / book_nodes /
//! book_attrs / book_chapters / book_string).

const std = @import("std");
pub const wire = @import("book_stream_wire.zig");

// ---- ra8_err_t (libs/ra8_core/inc/ra8_err.h, enum : uint16_t) --------------
const ra8_err_t = u16;
const k_ra8_ok: ra8_err_t = 0;
const k_ra8_err_invalid_arg: ra8_err_t = 0x103;
const k_ra8_err_invalid_size: ra8_err_t = 0x105;
const k_ra8_err_null_ptr: ra8_err_t = 0x504;

const k_book_sizeof_header: u32 = 100;
const k_book_sizeof_chapter: u32 = 12;
const k_book_sizeof_node: u32 = 24;
const k_book_sizeof_attr: u32 = 8;

/// Absent index (inc/book.h).
const k_book_nil: u32 = 0xFFFFFFFF;
const k_book_node_text: u8 = 1;

/// Loop-bound headroom for the iterative DOM walks (book_xhtml_bound_t,
/// book_internal.h, NASA Rule 2).
const k_book_xhtml_stack: u32 = 512;
const k_book_xhtml_iter_x: u32 = 4;

/// Log tag for serializer diagnostics (book_xhtml.c's s_tag_xhtml).
const s_tag_xhtml = "book_xhtml";

/// Host-order mirror of `book_node_t` (inc/book.h).
const BookNode = extern struct {
    kind: u8,
    reserved: u8,
    attr_count: u16,
    name_off: u32,
    text_off: u32,
    first_attr: u32,
    first_child: u32,
    next_sibling: u32,

    comptime {
        if (@sizeOf(@This()) != k_book_sizeof_node) {
            @compileError("book_node_t layout drift");
        }
    }
};

/// Host-order mirror of `book_chapter_t` (inc/book.h).
const BookChapter = extern struct {
    title_off: u32,
    href_off: u32,
    root_node: u32,

    comptime {
        if (@sizeOf(@This()) != k_book_sizeof_chapter) {
            @compileError("book_chapter_t layout drift");
        }
    }
};

/// Host-order mirror of `book_attr_t` (inc/book.h).
const BookAttr = extern struct {
    name_off: u32,
    value_off: u32,

    comptime {
        if (@sizeOf(@This()) != k_book_sizeof_attr) {
            @compileError("book_attr_t layout drift");
        }
    }
};

/// One deferred unit of work on the serializer stack: `is_close`
/// distinguishes a pending close tag (carrying the tag-name string offset)
/// from a node to open (carrying the node index).
const WalkEntry = extern struct {
    is_close: bool,
    value: u32,
};

/// Module-static walk stacks (book_xhtml.c kept them in function-static
/// storage so the frames stay within the stack-usage budget; the walks are
/// iterative and single-threaded, so the shared buffers never overlap).
var s_xhtml_stack: [k_book_xhtml_stack]WalkEntry = undefined;
var s_text_stack: [k_book_xhtml_stack]u32 = undefined;

// ---- Blob accessors (inc/book.h inline accessors) ---------------------------

/// View the blob base as its header: the first 100 bytes, copied into the
/// host-order mirror so field reads need no alignment.
fn readHeader(base: [*]const u8) wire.BookHeader {
    var hdr: wire.BookHeader = undefined;
    @memcpy(std.mem.asBytes(&hdr), base[0..k_book_sizeof_header]);
    return hdr;
}

/// Resolve a blob-relative byte offset to a pointer (book_at).
fn bookAt(base: [*]const u8, off: u32) [*]const u8 {
    return base + off;
}

/// Resolve a string-pool offset to a NUL-terminated UTF-8 string.
fn bookString(base: [*]const u8, hdr: wire.BookHeader, off: u32) [*:0]const u8 {
    return @ptrCast(bookAt(base, hdr.string_off + off));
}

/// Base of the DOM node table.
fn bookNodes(base: [*]const u8, hdr: wire.BookHeader) [*]const BookNode {
    return @ptrCast(@alignCast(bookAt(base, hdr.node_off)));
}

/// Base of the attribute table.
fn bookAttrs(base: [*]const u8, hdr: wire.BookHeader) [*]const BookAttr {
    return @ptrCast(@alignCast(bookAt(base, hdr.attr_off)));
}

/// Base of the chapter table.
fn bookChapters(base: [*]const u8, hdr: wire.BookHeader) [*]const BookChapter {
    return @ptrCast(@alignCast(bookAt(base, hdr.chapter_off)));
}

// ---- Raw append helpers ------------------------------------------------------

/// Append a raw byte span to the output buffer: copies exactly `src.len`
/// bytes at `pos.*`, advancing it on success. If the copy would exceed `cap`
/// the buffer is left unchanged and the function returns false so callers can
/// chain with `and` and abort the entire serialisation on first overflow.
fn internalEmit(out: [*]u8, cap: usize, pos: *usize, src: []const u8) bool {
    if (src.len > (cap - pos.*)) {
        return false;
    }
    @memcpy(out[pos.*..][0..src.len], src);
    pos.* += src.len;
    return true;
}

/// Append a NUL-terminated string to the output buffer (tag names, attribute
/// names, literal XML punctuation).
fn internalEmitCstr(out: [*]u8, cap: usize, pos: *usize, str: [*:0]const u8) bool {
    return internalEmit(out, cap, pos, std.mem.span(str));
}

/// Append entity-escaped text: `&` -> `&amp;`, `<` -> `&lt;`, `>` -> `&gt;`;
/// `"` becomes `&quot;` only when `in_attr` is true, otherwise the literal
/// `"` is emitted. All other characters are forwarded as-is. Stops on the
/// first overflow, leaving `pos.*` at the last successfully written position.
fn internalEmitEscaped(out: [*]u8, cap: usize, pos: *usize, str: [*:0]const u8, in_attr: bool) bool {
    for (std.mem.span(str)) |c| {
        const ok = switch (c) {
            '&' => internalEmit(out, cap, pos, "&amp;"),
            '<' => internalEmit(out, cap, pos, "&lt;"),
            '>' => internalEmit(out, cap, pos, "&gt;"),
            '"' => if (in_attr) internalEmit(out, cap, pos, "&quot;") else internalEmit(out, cap, pos, &[_]u8{c}),
            else => internalEmit(out, cap, pos, &[_]u8{c}),
        };
        if (!ok) {
            return false;
        }
    }
    return true;
}

/// Test whether an element name is an HTML void element (self-closes, may not
/// have child nodes in XHTML). Case-sensitive: compiled book DOM names are
/// already lower-cased during parsing.
fn internalIsVoid(name: [*:0]const u8) bool {
    const void_elements = [_][]const u8{
        "br",    "hr",    "img",    "meta",  "link", "input", "area", "base", "col",
        "embed", "param", "source", "track", "wbr",
    };
    const s = std.mem.span(name);
    for (void_elements) |v| {
        if (std.mem.eql(u8, s, v)) {
            return true;
        }
    }
    return false;
}

/// Append all attributes of a DOM node to the output buffer: for each
/// attribute slot a space, the name, `="` (with entity-escaped value) and a
/// closing `"`. Called while the opening tag is still open; the caller emits
/// the tag terminator after this returns.
fn internalEmitAttrs(base: [*]const u8, hdr: wire.BookHeader, node: *const BookNode, out: [*]u8, cap: usize, pos: *usize) bool {
    const attrs = bookAttrs(base, hdr);
    var i: u16 = 0;
    while (i < node.attr_count) : (i += 1) {
        const a = &attrs[node.first_attr + i];
        const ok = internalEmit(out, cap, pos, " ") and
            internalEmitCstr(out, cap, pos, bookString(base, hdr, a.name_off)) and
            internalEmit(out, cap, pos, "=\"") and
            internalEmitEscaped(out, cap, pos, bookString(base, hdr, a.value_off), true) and
            internalEmit(out, cap, pos, "\"");
        if (!ok) {
            return false;
        }
    }
    return true;
}

/// Emit an element open tag and schedule its close and children on the walk
/// stack: `<`, the tag name, all attributes, then `/>` for void elements or
/// `>` for the rest. For non-void elements two entries are pushed before
/// returning: first a close entry (so the closing tag is emitted after all
/// descendants), then a child-list entry (so the first child is processed
/// next by the outer loop in internalWalkToXhtml).
fn internalOpenElement(base: [*]const u8, hdr: wire.BookHeader, node: *const BookNode, out: [*]u8, cap: usize, pos: *usize, sp: *u32) bool {
    const name = bookString(base, hdr, node.name_off);
    if (!internalEmit(out, cap, pos, "<") or
        !internalEmitCstr(out, cap, pos, name) or
        !internalEmitAttrs(base, hdr, node, out, cap, pos))
    {
        return false;
    }
    if (internalIsVoid(name)) {
        return internalEmit(out, cap, pos, "/>");
    }
    if (!internalEmit(out, cap, pos, ">") or ((sp.* + 2) > k_book_xhtml_stack)) {
        return false;
    }
    s_xhtml_stack[sp.*] = .{ .is_close = true, .value = node.name_off }; // close after children
    sp.* += 1;
    s_xhtml_stack[sp.*] = .{ .is_close = false, .value = node.first_child }; // children first
    sp.* += 1;
    return true;
}

/// Iterative, bounded DOM walk that serialises a subtree to XHTML:
/// depth-first over the subtree rooted at `root` without recursion. The
/// module-static walk stack (maximum depth k_book_xhtml_stack) holds deferred
/// work: each entry is either a node-to-open or a pending close tag. An
/// iteration guard capped at `node_count * k_book_xhtml_iter_x +
/// k_book_xhtml_stack` prevents unbounded execution even if the DOM contains
/// cycles. Element nodes delegate opening (and child/close scheduling) to
/// internalOpenElement; text nodes are emitted via internalEmitEscaped.
fn internalWalkToXhtml(base: [*]const u8, hdr: wire.BookHeader, root: u32, node_count: u32, out: [*]u8, cap: usize, pos: *usize) bool {
    const nodes = bookNodes(base, hdr);
    var sp: u32 = 0;
    var ok = true;
    s_xhtml_stack[sp] = .{ .is_close = false, .value = root };
    sp += 1;

    const max_iter: u32 = (node_count *% k_book_xhtml_iter_x) +% k_book_xhtml_stack;
    var guard: u32 = 0;
    while ((sp > 0) and ok and (guard < max_iter)) {
        guard += 1;
        sp -= 1;
        const e = s_xhtml_stack[sp];
        if (e.is_close) {
            ok = internalEmit(out, cap, pos, "</") and
                internalEmitCstr(out, cap, pos, bookString(base, hdr, e.value)) and
                internalEmit(out, cap, pos, ">");
            continue;
        }
        if (e.value == k_book_nil) {
            continue;
        }
        const node = &nodes[e.value];
        if (sp >= k_book_xhtml_stack) {
            return false;
        }
        // Continue this level's sibling chain after the whole subtree + close.
        s_xhtml_stack[sp] = .{ .is_close = false, .value = node.next_sibling };
        sp += 1;
        if (node.kind == k_book_node_text) {
            ok = internalEmitEscaped(out, cap, pos, bookString(base, hdr, node.text_off), false);
        } else {
            ok = internalOpenElement(base, hdr, node, out, cap, pos, &sp);
        }
    }
    return ok and (guard < max_iter);
}

// ---- Shared leaf seams (book_internal.h RA8_PRIV surface) --------------------

/// Test whether an element name is a block-level HTML element, used to decide
/// where to insert paragraph-break newlines during plain-text extraction.
/// Case-sensitive (compiled book DOM names are lower-cased during parsing).
pub export fn priv_book_is_block(name: [*:0]const u8) callconv(.c) bool {
    const block_elements = [_][]const u8{
        "p",      "h1",         "h2",      "h3",    "h4",     "h5",      "h6",         "li",
        "ul",     "ol",         "div",     "br",    "hr",     "section", "tr",         "pre",
        "header", "blockquote", "article", "aside", "footer", "figure",  "figcaption",
    };
    const s = std.mem.span(name);
    for (block_elements) |b| {
        if (std.mem.eql(u8, s, b)) {
            return true;
        }
    }
    return false;
}

/// Append a whitespace-collapsed text run to the output buffer: any run of
/// space, tab, CR or LF folds into at most a single ASCII space; non-space
/// characters pass through unchanged. `at_break` carries inter-call state:
/// when true, leading whitespace in the fragment is silently dropped (so
/// indentation between inline elements does not leak into the rendered
/// prose). It is set true after a collapsed space and false after any
/// non-whitespace byte.
pub export fn priv_book_emit_text(out: [*]u8, cap: usize, pos: *usize, str: [*:0]const u8, at_break: *bool) callconv(.c) bool {
    for (std.mem.span(str)) |c| {
        const ws = (c == ' ') or (c == '\t') or (c == '\n') or (c == '\r');
        if (!ws) {
            if (!internalEmit(out, cap, pos, &[_]u8{c})) {
                return false;
            }
            at_break.* = false;
            continue;
        }
        if (at_break.*) {
            continue;
        }
        if (!internalEmit(out, cap, pos, " ")) {
            return false;
        }
        at_break.* = true;
    }
    return true;
}

/// Append a paragraph break, collapsing consecutive block-level breaks: trims
/// any trailing spaces already written, sets `at_break` to suppress leading
/// whitespace next, then appends a single '\n' unless the last byte is
/// already '\n' (so nested or adjacent block elements collapse to one blank
/// line).
pub export fn priv_book_emit_break(out: [*]u8, cap: usize, pos: *usize, at_break: *bool) callconv(.c) bool {
    while ((pos.* > 0) and (out[pos.* - 1] == ' ')) {
        pos.* -= 1;
    }
    at_break.* = true;
    if ((pos.* > 0) and (out[pos.* - 1] == '\n')) {
        return true;
    }
    return internalEmit(out, cap, pos, "\n");
}

/// Bounded pre-order walk that extracts plain text from a DOM subtree:
/// text nodes are handed to priv_book_emit_text for whitespace collapsing;
/// block-level elements trigger priv_book_emit_break before descending; all
/// other elements descend immediately with no separator. The `at_break` flag
/// threads through all text and break calls so leading whitespace at the
/// start of each block is suppressed.
fn internalWalkText(base: [*]const u8, hdr: wire.BookHeader, root: u32, node_count: u32, out: [*]u8, cap: usize, pos: *usize) bool {
    const nodes = bookNodes(base, hdr);
    var sp: u32 = 0;
    var ok = true;
    var at_break = true;
    s_text_stack[sp] = root;
    sp += 1;

    const max_iter: u32 = (node_count *% k_book_xhtml_iter_x) +% k_book_xhtml_stack;
    var guard: u32 = 0;
    while ((sp > 0) and ok and (guard < max_iter)) {
        guard += 1;
        sp -= 1;
        const n = s_text_stack[sp];
        if (n == k_book_nil) {
            continue;
        }
        const node = &nodes[n];
        if (sp >= k_book_xhtml_stack) {
            return false;
        }
        s_text_stack[sp] = node.next_sibling; // sibling chain after this subtree
        sp += 1;
        if (node.kind == k_book_node_text) {
            ok = priv_book_emit_text(out, cap, pos, bookString(base, hdr, node.text_off), &at_break);
            continue;
        }
        if (priv_book_is_block(bookString(base, hdr, node.name_off))) {
            ok = priv_book_emit_break(out, cap, pos, &at_break);
        }
        if (ok and (sp < k_book_xhtml_stack)) {
            s_text_stack[sp] = node.first_child; // descend, pre-order
            sp += 1;
        }
    }
    return ok and (guard < max_iter);
}

// ---- Public API (inc/book.h) --------------------------------------------------

/// Extract the plain text of one chapter: whitespace-collapsed runs with a
/// single '\n' at each block boundary.
pub export fn book_chapter_text(base: ?[*]const u8, chapter_idx: u32, out: ?[*]u8, cap: usize, out_len: ?*usize) callconv(.c) ra8_err_t {
    if (base == null) return k_ra8_err_null_ptr; // text: null base
    if (out == null) return k_ra8_err_null_ptr; // text: null out
    if (out_len == null) return k_ra8_err_null_ptr; // text: null out_len

    const b = base.?;
    const hdr = readHeader(b);
    if (chapter_idx >= hdr.chapter_count) {
        return k_ra8_err_invalid_arg;
    }
    const root = bookChapters(b, hdr)[chapter_idx].root_node;
    var pos: usize = 0;
    if (!internalWalkText(b, hdr, root, hdr.node_count, out.?, cap, &pos)) {
        return k_ra8_err_invalid_size;
    }
    out_len.?.* = pos;
    return k_ra8_ok;
}

/// Serialize one chapter's DOM back to XHTML for the renderer.
pub export fn book_chapter_to_xhtml(base: ?[*]const u8, chapter_idx: u32, out: ?[*]u8, cap: usize, out_len: ?*usize) callconv(.c) ra8_err_t {
    if (base == null) return k_ra8_err_null_ptr; // to_xhtml: null base
    if (out == null) return k_ra8_err_null_ptr; // to_xhtml: null out
    if (out_len == null) return k_ra8_err_null_ptr; // to_xhtml: null out_len

    const b = base.?;
    const hdr = readHeader(b);
    if (chapter_idx >= hdr.chapter_count) {
        return k_ra8_err_invalid_arg;
    }
    const root = bookChapters(b, hdr)[chapter_idx].root_node;
    var pos: usize = 0;
    if (!internalWalkToXhtml(b, hdr, root, hdr.node_count, out.?, cap, &pos)) {
        return k_ra8_err_invalid_size;
    }
    out_len.?.* = pos;
    return k_ra8_ok;
}
