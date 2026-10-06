//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const writer = @import("writer");

const Harness = struct {
    buffer: [512]u8 = undefined,
    frames: [8]writer.Frame = undefined,

    fn open(self: *Harness) writer.Writer {
        return writer.Writer.init(&self.buffer, &self.frames) catch unreachable;
    }
};

test "init refuses empty storage" {
    var buffer: [8]u8 = undefined;
    var frames: [2]writer.Frame = undefined;
    try std.testing.expectError(writer.Error.InvalidArgument, writer.Writer.init(buffer[0..0], &frames));
    try std.testing.expectError(writer.Error.InvalidArgument, writer.Writer.init(&buffer, frames[0..0]));
}

test "a document of declaration, attributes and text" {
    var harness = Harness{};
    var w = harness.open();

    try w.declaration();
    try w.startElement("package");
    try w.attr("version", "3.0");
    try w.startElement("title");
    try w.text("Fire & Ice");
    try w.endElement();
    try w.endElement();

    const length = try w.finish();
    try std.testing.expectEqualStrings(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>" ++
            "<package version=\"3.0\"><title>Fire &amp; Ice</title></package>",
        w.written(),
    );
    try std.testing.expectEqual(w.written().len, length);
}

test "an element that took nothing closes as an empty element" {
    var harness = Harness{};
    var w = harness.open();

    try w.startElement("meta");
    try w.attr("name", "cover");
    try w.endElement();

    _ = try w.finish();
    try std.testing.expectEqualStrings("<meta name=\"cover\"/>", w.written());
}

test "attribute values escape all five, not just the three text needs" {
    var harness = Harness{};
    var w = harness.open();

    try w.startElement("a");
    try w.attr("v", "<&>\"'");
    try w.endElement();

    _ = try w.finish();
    try std.testing.expectEqualStrings("<a v=\"&lt;&amp;&gt;&quot;&apos;\"/>", w.written());
}

test "the declaration is legal only as the first bytes" {
    var harness = Harness{};
    var w = harness.open();

    try w.startElement("root");
    try std.testing.expectError(writer.Error.InvalidState, w.declaration());
}

test "failure is sticky and every later call repeats it" {
    var harness = Harness{};
    var w = harness.open();

    try std.testing.expectError(writer.Error.InvalidArgument, w.startElement("1bad"));
    try std.testing.expectError(writer.Error.InvalidArgument, w.startElement("good"));
    try std.testing.expectError(writer.Error.InvalidArgument, w.text("anything"));
    try std.testing.expectError(writer.Error.InvalidArgument, w.finish());
}

test "text outside any element is refused" {
    var harness = Harness{};
    var w = harness.open();
    try std.testing.expectError(writer.Error.InvalidState, w.text("loose"));
}

test "an attribute after content is refused" {
    var harness = Harness{};
    var w = harness.open();

    try w.startElement("a");
    try w.text("content");
    try std.testing.expectError(writer.Error.InvalidState, w.attr("late", "1"));
}

test "closing more elements than were opened is refused" {
    var harness = Harness{};
    var w = harness.open();

    try w.startElement("a");
    try w.endElement();
    try std.testing.expectError(writer.Error.InvalidState, w.endElement());
}

test "finishing with an element still open is refused" {
    var harness = Harness{};
    var w = harness.open();

    try w.startElement("a");
    try w.text("x");
    try std.testing.expectError(writer.Error.InvalidState, w.finish());
}

test "the frame stack bounds nesting depth" {
    var buffer: [128]u8 = undefined;
    var frames: [2]writer.Frame = undefined;
    var w = try writer.Writer.init(&buffer, &frames);

    try w.startElement("a");
    try w.startElement("b");
    try std.testing.expectError(writer.Error.NoSpace, w.startElement("c"));
}

test "a name longer than a frame holds is refused" {
    var harness = Harness{};
    var w = harness.open();

    const long = &@as([64:0]u8, @splat('a'));
    try std.testing.expectError(writer.Error.NoSpace, w.startElement(long));
}

test "a name that exactly fills a frame is accepted and closed by name" {
    var harness = Harness{};
    var w = harness.open();

    const exact = &@as([63:0]u8, @splat('a'));
    try w.startElement(exact);
    try w.text("x");
    try w.endElement();

    _ = try w.finish();
    try std.testing.expectEqualStrings("<" ++ exact ++ ">x</" ++ exact ++ ">", w.written());
}

test "a document that overruns its buffer refuses and stays refused" {
    var buffer: [16]u8 = undefined;
    var frames: [4]writer.Frame = undefined;
    var w = try writer.Writer.init(&buffer, &frames);

    try std.testing.expectError(writer.Error.NoSpace, w.declaration());
    try std.testing.expectError(writer.Error.NoSpace, w.startElement("a"));
}

test "names are copied, so a reused scratch buffer is safe" {
    var harness = Harness{};
    var w = harness.open();

    var scratch: [8]u8 = undefined;
    @memcpy(scratch[0..5], "outer");
    try w.startElement(scratch[0..5]);
    @memcpy(scratch[0..5], "XXXXX");

    try w.endElement();
    _ = try w.finish();
    try std.testing.expectEqualStrings("<outer/>", w.written());
}

test "the buffer holds a terminator after every accepted append" {
    var harness = Harness{};
    var w = harness.open();

    try w.startElement("a");
    try std.testing.expectEqual(@as(u8, 0), harness.buffer[w.len]);
    try w.endElement();
    try std.testing.expectEqual(@as(u8, 0), harness.buffer[w.len]);
}
