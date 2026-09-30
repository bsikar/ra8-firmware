//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const abi = @import("abi");

extern fn ra8_xml_escape(src: ?[*:0]const u8, out: ?[*]u8, cap: usize) callconv(.c) abi.XmlError;
extern fn ra8_xml_writer_init(
    w: ?*abi.Writer,
    out: ?[*]u8,
    cap: usize,
    frames: ?[*]abi.Frame,
    frame_cap: u32,
) callconv(.c) abi.XmlError;
extern fn ra8_xml_writer_declaration(w: ?*abi.Writer) callconv(.c) abi.XmlError;
extern fn ra8_xml_writer_start_element(w: ?*abi.Writer, tag: ?[*:0]const u8) callconv(.c) abi.XmlError;
extern fn ra8_xml_writer_attr(
    w: ?*abi.Writer,
    key: ?[*:0]const u8,
    value: ?[*:0]const u8,
) callconv(.c) abi.XmlError;
extern fn ra8_xml_writer_text(w: ?*abi.Writer, content: ?[*:0]const u8) callconv(.c) abi.XmlError;
extern fn ra8_xml_writer_end_element(w: ?*abi.Writer) callconv(.c) abi.XmlError;
extern fn ra8_xml_writer_finish(w: ?*abi.Writer, out_len: ?*usize) callconv(.c) abi.XmlError;

const Harness = struct {
    writer: abi.Writer = undefined,
    buffer: [512]u8 = undefined,
    frames: [8]abi.Frame = undefined,

    fn arm(self: *Harness) *abi.Writer {
        const err = ra8_xml_writer_init(&self.writer, &self.buffer, self.buffer.len, &self.frames, self.frames.len);
        std.debug.assert(err == .ok);
        return &self.writer;
    }

    fn document(self: *const Harness) []const u8 {
        return self.buffer[0..self.writer.len];
    }
};

test "the published struct layout is what the header declares" {
    const word = @sizeOf(usize);
    try std.testing.expectEqual(@as(usize, 64), @sizeOf(abi.Frame));
    try std.testing.expectEqual(word * 4 + 8, @offsetOf(abi.Writer, "status"));
    try std.testing.expectEqual(@as(usize, 2), @sizeOf(abi.XmlError));
}

test "escape writes an escaped, terminated string" {
    var out: [64]u8 = undefined;
    try std.testing.expectEqual(abi.XmlError.ok, ra8_xml_escape("a&b", &out, out.len));
    try std.testing.expectEqualStrings("a&amp;b", std.mem.sliceTo(&out, 0));
}

test "escape refuses null arguments and a zero capacity" {
    var out: [8]u8 = undefined;
    try std.testing.expectEqual(abi.XmlError.invalid_arg, ra8_xml_escape("a", null, 8));
    try std.testing.expectEqual(abi.XmlError.invalid_arg, ra8_xml_escape("a", &out, 0));
    try std.testing.expectEqual(abi.XmlError.invalid_arg, ra8_xml_escape(null, &out, out.len));

    // A refused call still leaves an empty string behind, never a fragment.
    try std.testing.expectEqual(@as(u8, 0), out[0]);
}

test "escape leaves nothing behind when the result does not fit" {
    var out: [4]u8 = undefined;
    out[0] = 'x';
    try std.testing.expectEqual(abi.XmlError.no_mem, ra8_xml_escape("&", &out, out.len));
    try std.testing.expectEqual(@as(u8, 0), out[0]);
}

test "init refuses null arguments and empty capacities" {
    var harness = Harness{};
    try std.testing.expectEqual(
        abi.XmlError.invalid_arg,
        ra8_xml_writer_init(null, &harness.buffer, 8, &harness.frames, 2),
    );
    try std.testing.expectEqual(
        abi.XmlError.invalid_arg,
        ra8_xml_writer_init(&harness.writer, null, 8, &harness.frames, 2),
    );
    try std.testing.expectEqual(
        abi.XmlError.invalid_arg,
        ra8_xml_writer_init(&harness.writer, &harness.buffer, 8, null, 2),
    );
    try std.testing.expectEqual(
        abi.XmlError.invalid_arg,
        ra8_xml_writer_init(&harness.writer, &harness.buffer, 0, &harness.frames, 2),
    );
    try std.testing.expectEqual(
        abi.XmlError.invalid_arg,
        ra8_xml_writer_init(&harness.writer, &harness.buffer, 8, &harness.frames, 0),
    );
}

test "init arms the writer and empties the document" {
    var harness = Harness{};
    harness.buffer[0] = 'x';
    const w = harness.arm();
    try std.testing.expect(w.ready);
    try std.testing.expectEqual(abi.XmlError.ok, w.status);
    try std.testing.expectEqual(@as(usize, 0), w.len);
    try std.testing.expectEqual(@as(u8, 0), harness.buffer[0]);
}

test "an unarmed writer reports invalid state, not its zeroed status" {
    var cold = std.mem.zeroes(abi.Writer);
    try std.testing.expectEqual(abi.XmlError.invalid_state, ra8_xml_writer_declaration(&cold));
    try std.testing.expectEqual(abi.XmlError.invalid_state, ra8_xml_writer_start_element(&cold, "a"));
    try std.testing.expectEqual(abi.XmlError.invalid_state, ra8_xml_writer_end_element(&cold));
    try std.testing.expectEqual(abi.XmlError.invalid_state, ra8_xml_writer_finish(&cold, null));

    // A null string argument on an unarmed writer is still invalid state.
    try std.testing.expectEqual(abi.XmlError.invalid_state, ra8_xml_writer_text(&cold, null));
}

test "every entry point refuses a null writer" {
    try std.testing.expectEqual(abi.XmlError.invalid_arg, ra8_xml_writer_declaration(null));
    try std.testing.expectEqual(abi.XmlError.invalid_arg, ra8_xml_writer_start_element(null, "a"));
    try std.testing.expectEqual(abi.XmlError.invalid_arg, ra8_xml_writer_attr(null, "a", "b"));
    try std.testing.expectEqual(abi.XmlError.invalid_arg, ra8_xml_writer_text(null, "a"));
    try std.testing.expectEqual(abi.XmlError.invalid_arg, ra8_xml_writer_end_element(null));
    try std.testing.expectEqual(abi.XmlError.invalid_arg, ra8_xml_writer_finish(null, null));
}

test "a whole document through the C entry points" {
    var harness = Harness{};
    const w = harness.arm();

    try std.testing.expectEqual(abi.XmlError.ok, ra8_xml_writer_declaration(w));
    try std.testing.expectEqual(abi.XmlError.ok, ra8_xml_writer_start_element(w, "package"));
    try std.testing.expectEqual(abi.XmlError.ok, ra8_xml_writer_attr(w, "version", "3.0"));
    try std.testing.expectEqual(abi.XmlError.ok, ra8_xml_writer_start_element(w, "title"));
    try std.testing.expectEqual(abi.XmlError.ok, ra8_xml_writer_text(w, "Fire & Ice"));
    try std.testing.expectEqual(abi.XmlError.ok, ra8_xml_writer_end_element(w));
    try std.testing.expectEqual(abi.XmlError.ok, ra8_xml_writer_end_element(w));

    var length: usize = 0;
    try std.testing.expectEqual(abi.XmlError.ok, ra8_xml_writer_finish(w, &length));
    try std.testing.expectEqualStrings(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>" ++
            "<package version=\"3.0\"><title>Fire &amp; Ice</title></package>",
        harness.document(),
    );
    try std.testing.expectEqual(harness.document().len, length);
    try std.testing.expectEqual(@as(u8, 0), harness.buffer[length]);
}

test "finish clears the caller's length slot before it can refuse" {
    var harness = Harness{};
    const w = harness.arm();
    try std.testing.expectEqual(abi.XmlError.ok, ra8_xml_writer_start_element(w, "a"));

    var length: usize = 99;
    try std.testing.expectEqual(abi.XmlError.invalid_state, ra8_xml_writer_finish(w, &length));
    try std.testing.expectEqual(@as(usize, 0), length);
}

test "a null name records the sticky refusal the C recorded" {
    var harness = Harness{};
    const w = harness.arm();

    try std.testing.expectEqual(abi.XmlError.invalid_arg, ra8_xml_writer_start_element(w, null));
    try std.testing.expectEqual(abi.XmlError.invalid_arg, w.status);
    try std.testing.expectEqual(abi.XmlError.invalid_arg, ra8_xml_writer_start_element(w, "good"));
}

test "a null attribute value is refused before the tag-open check" {
    var harness = Harness{};
    const w = harness.arm();
    try std.testing.expectEqual(abi.XmlError.invalid_arg, ra8_xml_writer_attr(w, "k", null));
    try std.testing.expectEqual(abi.XmlError.invalid_arg, w.status);
}

test "refusals are sticky across the membrane" {
    var harness = Harness{};
    const w = harness.arm();

    try std.testing.expectEqual(abi.XmlError.invalid_state, ra8_xml_writer_text(w, "loose"));
    try std.testing.expectEqual(abi.XmlError.invalid_state, w.status);
    try std.testing.expectEqual(abi.XmlError.invalid_state, ra8_xml_writer_start_element(w, "a"));
    try std.testing.expectEqual(abi.XmlError.invalid_state, ra8_xml_writer_finish(w, null));
}

test "init on a used writer restarts it without carrying the failure" {
    var harness = Harness{};
    const w = harness.arm();
    try std.testing.expectEqual(abi.XmlError.invalid_state, ra8_xml_writer_text(w, "loose"));

    _ = harness.arm();
    try std.testing.expectEqual(abi.XmlError.ok, w.status);
    try std.testing.expectEqual(abi.XmlError.ok, ra8_xml_writer_start_element(w, "a"));
    try std.testing.expectEqual(abi.XmlError.ok, ra8_xml_writer_end_element(w));
    try std.testing.expectEqualStrings("<a/>", harness.document());
}

test "an overrun refuses with no memory and leaves the accepted prefix" {
    var harness = Harness{};
    const err = ra8_xml_writer_init(&harness.writer, &harness.buffer, 6, &harness.frames, harness.frames.len);
    try std.testing.expectEqual(abi.XmlError.ok, err);

    try std.testing.expectEqual(abi.XmlError.ok, ra8_xml_writer_start_element(&harness.writer, "ab"));
    try std.testing.expectEqualStrings("<ab", harness.document());
    try std.testing.expectEqual(abi.XmlError.no_mem, ra8_xml_writer_text(&harness.writer, "cdef"));
}

test "depth is bounded by the caller's frame count" {
    var harness = Harness{};
    const err = ra8_xml_writer_init(&harness.writer, &harness.buffer, harness.buffer.len, &harness.frames, 2);
    try std.testing.expectEqual(abi.XmlError.ok, err);

    try std.testing.expectEqual(abi.XmlError.ok, ra8_xml_writer_start_element(&harness.writer, "a"));
    try std.testing.expectEqual(abi.XmlError.ok, ra8_xml_writer_start_element(&harness.writer, "b"));
    try std.testing.expectEqual(abi.XmlError.no_mem, ra8_xml_writer_start_element(&harness.writer, "c"));
}
