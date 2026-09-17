//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Behavioural regression suite for the record algebra behind `merge_ihex`
//! (#858). These cases pin what the replaced Python tool did: EOF records are
//! dropped, every other record survives in order, one canonical EOF closes the
//! image, and malformed or blank input is passed over rather than rejected.

const std = @import("std");
const merge_ihex = @import("implementation");

test "isRecord accepts colon-led lines only" {
    try std.testing.expect(merge_ihex.isRecord(":10000000FF"));
    try std.testing.expect(!merge_ihex.isRecord(""));
    try std.testing.expect(!merge_ihex.isRecord("# comment"));
}

test "isEofRecord matches the type field, not the text" {
    try std.testing.expect(merge_ihex.isEofRecord(":00000001FF"));
    try std.testing.expect(!merge_ihex.isEofRecord(":020000040208F0"));
    try std.testing.expect(!merge_ihex.isEofRecord(":1000000001FF"));
}

test "isEofRecord is case insensitive in the type field" {
    try std.testing.expect(merge_ihex.isEofRecord(":00000001ff"));
}

test "isEofRecord keeps records too short to carry a type field" {
    try std.testing.expect(!merge_ihex.isEofRecord(":0000"));
}

test "appendDataRecords drops EOF records and blank filler" {
    var records = std.ArrayList([]const u8).init(std.testing.allocator);
    defer records.deinit();

    try merge_ihex.appendDataRecords(&records,
        \\:020000040200F8
        \\
        \\:10000000AABBCCDD11223344556677889900AABB33
        \\not a record
        \\:00000001FF
        \\
    );

    try std.testing.expectEqual(@as(usize, 2), records.items.len);
    try std.testing.expectEqualStrings(":020000040200F8", records.items[0]);
    try std.testing.expectEqualStrings(":10000000AABBCCDD11223344556677889900AABB33", records.items[1]);
}

test "appendDataRecords strips surrounding whitespace, carriage returns included" {
    var records = std.ArrayList([]const u8).init(std.testing.allocator);
    defer records.deinit();

    try merge_ihex.appendDataRecords(&records, "  :020000040200F8\r\n\t:00000001FF\r\n");

    try std.testing.expectEqual(@as(usize, 1), records.items.len);
    try std.testing.expectEqualStrings(":020000040200F8", records.items[0]);
}

test "appendDataRecords splits on a lone carriage return, as Python text mode did" {
    var records = std.ArrayList([]const u8).init(std.testing.allocator);
    defer records.deinit();

    try merge_ihex.appendDataRecords(&records, ":020000040200F8\r:10000000AA33\r:00000001FF\r");

    try std.testing.expectEqual(@as(usize, 2), records.items.len);
    try std.testing.expectEqualStrings(":020000040200F8", records.items[0]);
    try std.testing.expectEqualStrings(":10000000AA33", records.items[1]);
}

test "merge concatenates both images and terminates once" {
    const merged = try merge_ihex.merge(
        std.testing.allocator,
        ":020000040200F8\n:10000000AA33\n:00000001FF\n",
        ":020000040208F0\n:10000000BB22\n:00000001FF\n",
    );
    defer merged.deinit(std.testing.allocator);

    try std.testing.expectEqual(@as(usize, 4), merged.record_count);
    try std.testing.expectEqualStrings(
        ":020000040200F8\n:10000000AA33\n:020000040208F0\n:10000000BB22\n:00000001FF\n",
        merged.text,
    );
}

test "merge of two empty images is still a valid terminated image" {
    const merged = try merge_ihex.merge(std.testing.allocator, "", "");
    defer merged.deinit(std.testing.allocator);

    try std.testing.expectEqual(@as(usize, 0), merged.record_count);
    try std.testing.expectEqualStrings(":00000001FF\n", merged.text);
}

test "merge preserves each input's extended linear address records" {
    const merged = try merge_ihex.merge(
        std.testing.allocator,
        ":020000040200F8\n:00000001FF\n",
        ":020000040208F0\n:00000001FF\n",
    );
    defer merged.deinit(std.testing.allocator);

    try std.testing.expect(std.mem.indexOf(u8, merged.text, ":020000040200F8") != null);
    try std.testing.expect(std.mem.indexOf(u8, merged.text, ":020000040208F0") != null);
}

test "merged image ends with the canonical EOF record and a newline" {
    const merged = try merge_ihex.merge(std.testing.allocator, ":10000000AA33\n", "");
    defer merged.deinit(std.testing.allocator);

    try std.testing.expect(std.mem.endsWith(u8, merged.text, merge_ihex.eof_record ++ "\n"));
    try std.testing.expectEqual(
        @as(usize, 1),
        std.mem.count(u8, merged.text, merge_ihex.eof_record),
    );
}
