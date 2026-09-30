//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const entity = @import("entity");

test "every metacharacter has an entity form and nothing else does" {
    try std.testing.expectEqualStrings("&amp;", entity.form('&').?);
    try std.testing.expectEqualStrings("&lt;", entity.form('<').?);
    try std.testing.expectEqualStrings("&gt;", entity.form('>').?);
    try std.testing.expectEqualStrings("&quot;", entity.form('"').?);
    try std.testing.expectEqualStrings("&apos;", entity.form('\'').?);

    for ("azAZ09 _-.:/\\\n\t") |byte| try std.testing.expect(entity.form(byte) == null);
}

test "the longest entity fits the published cap" {
    try std.testing.expectEqual(@as(usize, 6), entity.form('"').?.len);
    try std.testing.expect(entity.form('"').?.len <= entity.limits.entity_bytes);
}

test "escaped length is what escaping writes" {
    const cases = [_][]const u8{ "", "plain", "a&b", "<<>>", "\"'\"'", "mixed <tag attr=\"v\">" };
    var buffer: [128]u8 = undefined;
    for (cases) |case| {
        const written = try entity.escape(case, &buffer);
        try std.testing.expectEqual(entity.escapedLen(case), written.len);
    }
}

test "escaping substitutes all five in one pass" {
    var buffer: [64]u8 = undefined;
    const written = try entity.escape("a&b<c>d\"e'f", &buffer);
    try std.testing.expectEqualStrings("a&amp;b&lt;c&gt;d&quot;e&apos;f", written);
}

test "a value that does not fit is refused whole" {
    var buffer: [4]u8 = undefined;
    try std.testing.expectError(error.NoSpace, entity.escape("&", &buffer));

    // Exactly enough is not a refusal.
    var exact: [5]u8 = undefined;
    try std.testing.expectEqualStrings("&amp;", try entity.escape("&", &exact));
}

test "an empty source escapes to an empty result" {
    var buffer: [1]u8 = undefined;
    try std.testing.expectEqual(@as(usize, 0), (try entity.escape("", &buffer)).len);
}
