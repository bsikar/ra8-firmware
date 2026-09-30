//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The bounded XML emitter: pure string construction over caller-owned
//! storage. No allocation, no recursion, no global state.
//!
//! Every append is length-checked before a byte is written, so a refused call
//! leaves the document exactly as the last accepted call left it. Failure is
//! sticky: the first refusal is remembered and every later call is a no-op
//! returning it, which is what lets a caller test once at `finish` instead of
//! after each append.

const std = @import("std");

const entity = @import("entity.zig");
const name = @import("name.zig");

/// Why an append was refused. Mapped to `ra8_err_t` at the membrane.
pub const Error = error{
    /// The document or the frame stack is full.
    NoSpace,
    /// A name, a value or a pointer the caller supplied is unusable.
    InvalidArgument,
    /// The call is not legal where the document currently stands.
    InvalidState,
};

/// One open element. The name is copied, not borrowed, so a document may be
/// built from names assembled in a scratch buffer that is overwritten between
/// calls.
pub const Frame = extern struct {
    name: [name.limits.name_cap]u8,

    /// The stored name as a slice, terminator excluded.
    pub fn text(self: *const Frame) []const u8 {
        return self.name[0..std.mem.indexOfScalar(u8, &self.name, 0).?];
    }
};

/// The one declaration every document in this tree carries.
pub const declaration_text = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>";

/// Builder state over a caller-owned buffer and element stack.
pub const Writer = struct {
    out: []u8,
    frames: []Frame,
    len: usize = 0,
    depth: usize = 0,
    status: ?Error = null,
    tag_open: bool = false,

    /// Bind a writer to caller storage and clear it to an empty document.
    pub fn init(out: []u8, frames: []Frame) Error!Writer {
        if (out.len == 0 or frames.len == 0) return Error.InvalidArgument;
        out[0] = 0;
        return .{ .out = out, .frames = frames };
    }

    /// Emit the XML declaration. Legal only as the document's first bytes.
    pub fn declaration(self: *Writer) Error!void {
        try self.usable();
        if (self.depth != 0 or self.len != 0) return self.fail(Error.InvalidState);
        try self.put(declaration_text);
    }

    /// Open an element, leaving its start tag ready to take attributes.
    pub fn startElement(self: *Writer, tag: []const u8) Error!void {
        try self.usable();
        if (!name.isValid(tag)) return self.fail(Error.InvalidArgument);
        if (tag.len > name.limits.name_bytes) return self.fail(Error.NoSpace);
        if (self.depth >= self.frames.len) return self.fail(Error.NoSpace);

        try self.closeTag();
        try self.put("<");
        try self.put(tag);

        const frame = &self.frames[self.depth];
        @memcpy(frame.name[0..tag.len], tag);
        frame.name[tag.len] = 0;
        self.depth += 1;
        self.tag_open = true;
    }

    /// Add an attribute to the start tag still being written.
    pub fn attr(self: *Writer, key: []const u8, value: []const u8) Error!void {
        try self.usable();
        if (!name.isValid(key)) return self.fail(Error.InvalidArgument);
        if (!self.tag_open) return self.fail(Error.InvalidState);

        try self.put(" ");
        try self.put(key);
        try self.put("=\"");
        try self.putEscaped(value);
        try self.put("\"");
    }

    /// Append escaped character data inside the open element.
    pub fn text(self: *Writer, content: []const u8) Error!void {
        try self.usable();
        if (self.depth == 0) return self.fail(Error.InvalidState);
        try self.closeTag();
        try self.putEscaped(content);
    }

    /// Close the innermost open element, as `<name/>` when it stayed empty.
    pub fn endElement(self: *Writer) Error!void {
        try self.usable();
        if (self.depth == 0) return self.fail(Error.InvalidState);

        if (self.tag_open) {
            try self.put("/>");
            self.tag_open = false;
            self.depth -= 1;
            return;
        }
        try self.put("</");
        try self.put(self.frames[self.depth - 1].text());
        try self.put(">");
        self.depth -= 1;
    }

    /// Settle the document and hand back its length.
    pub fn finish(self: *Writer) Error!usize {
        if (self.status) |err| return err;
        if (self.depth != 0 or self.tag_open) return self.fail(Error.InvalidState);
        return self.len;
    }

    /// The document written so far, terminator excluded.
    pub fn written(self: *const Writer) []const u8 {
        return self.out[0..self.len];
    }

    /// Refuse unless the builder is armed and has not yet refused anything.
    fn usable(self: *Writer) Error!void {
        if (self.status) |err| return err;
    }

    /// Record the first refusal and hand it back unchanged.
    fn fail(self: *Writer, err: Error) Error {
        if (self.status == null) self.status = err;
        return err;
    }

    /// Append `bytes`, or refuse without touching the document.
    fn put(self: *Writer, bytes: []const u8) Error!void {
        // One byte is always held back for the terminator the C ABI promises.
        if (self.len + bytes.len + 1 > self.out.len) return self.fail(Error.NoSpace);
        @memcpy(self.out[self.len..][0..bytes.len], bytes);
        self.len += bytes.len;
        self.out[self.len] = 0;
    }

    /// Append `bytes` with the five predefined entities substituted.
    ///
    /// Byte at a time rather than measure-then-copy, so a value that overruns
    /// leaves the prefix that fitted exactly as the C did.
    fn putEscaped(self: *Writer, bytes: []const u8) Error!void {
        for (bytes) |byte| {
            if (entity.form(byte)) |ent| {
                try self.put(ent);
            } else {
                try self.put(&[_]u8{byte});
            }
        }
    }

    /// Close a pending start tag with `>` so content may follow.
    fn closeTag(self: *Writer) Error!void {
        if (!self.tag_open) return;
        try self.put(">");
        self.tag_open = false;
    }
};
