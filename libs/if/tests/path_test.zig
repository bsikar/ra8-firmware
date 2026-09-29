//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for the untrusted-name policy core, driven directly so every
//! branch of the C implementation this replaces is reachable without a
//! filesystem.

const std = @import("std");
const path = @import("policy");

const testing = std.testing;

fn sanitize(raw: ?[]const u8, buffer: []u8) !path.Segment {
    return path.sanitizeSegment(raw, buffer);
}

test "allowedChar admits exactly the safe alphabet" {
    for ("AZaz09.-_") |c| try testing.expect(path.allowedChar(c));
    for ("/ \t\x00:*?\"<>|\\\x7f") |c| try testing.expect(!path.allowedChar(c));
}

test "dotSegment catches the three unusable names" {
    try testing.expect(path.dotSegment(""));
    try testing.expect(path.dotSegment("."));
    try testing.expect(path.dotSegment(".."));
    try testing.expect(!path.dotSegment("..."));
    try testing.expect(!path.dotSegment(".a"));
}

test "baseOf folds case and stops at the first dot" {
    var storage: [8]u8 = undefined;
    try testing.expectEqualStrings("con", path.baseOf("CON.txt", &storage));
    try testing.expectEqualStrings("readme", path.baseOf("README", &storage));
    try testing.expectEqualStrings("", path.baseOf(".hidden", &storage));
}

test "baseOf keeps at most the C classifier's seven bytes" {
    var storage: [8]u8 = undefined;
    try testing.expectEqualStrings("abcdefg", path.baseOf("abcdefghij", &storage));
}

test "reservedBase matches the exact device names case-folded" {
    for ([_][]const u8{ "con", "CON", "Prn.txt", "aux", "NUL.log" }) |name| {
        try testing.expect(path.reservedBase(name));
    }
}

test "reservedBase matches comN and lptN only for digits one to nine" {
    try testing.expect(path.reservedBase("COM1"));
    try testing.expect(path.reservedBase("lpt9.txt"));
    try testing.expect(!path.reservedBase("com0"));
    try testing.expect(!path.reservedBase("com10"));
    try testing.expect(!path.reservedBase("comx"));
    try testing.expect(!path.reservedBase("con1"));
}

test "sanitizeSegment passes a clean name through verbatim" {
    var buffer: [32]u8 = undefined;
    const segment = try sanitize("chapter-01.txt", &buffer);
    try testing.expect(segment.verbatim);
    try testing.expectEqualStrings("chapter-01.txt", buffer[0..segment.len]);
}

test "sanitizeSegment replaces every disallowed byte" {
    var buffer: [32]u8 = undefined;
    const segment = try sanitize("a/b c:d", &buffer);
    try testing.expect(!segment.verbatim);
    try testing.expectEqualStrings("a_b_c_d", buffer[0..segment.len]);
}

test "sanitizeSegment reports truncation as not verbatim" {
    var buffer: [4]u8 = undefined;
    const segment = try sanitize("abcdef", &buffer);
    try testing.expect(!segment.verbatim);
    try testing.expectEqualStrings("abc", buffer[0..segment.len]);
}

test "sanitizeSegment substitutes the fallback for a dot name" {
    var buffer: [16]u8 = undefined;
    for ([_][]const u8{ "", ".", ".." }) |raw| {
        const segment = try sanitize(raw, &buffer);
        try testing.expect(!segment.verbatim);
        try testing.expectEqualStrings("item", buffer[0..segment.len]);
    }
}

test "sanitizeSegment substitutes the fallback for a null candidate" {
    var buffer: [16]u8 = undefined;
    const segment = try sanitize(null, &buffer);
    try testing.expect(!segment.verbatim);
    try testing.expectEqualStrings("item", buffer[0..segment.len]);
}

test "sanitizeSegment clips the fallback to a tiny capacity" {
    var buffer: [3]u8 = undefined;
    const segment = try sanitize("..", &buffer);
    try testing.expectEqualStrings("it", buffer[0..segment.len]);
}

test "sanitizeSegment prefixes a reserved base" {
    var buffer: [16]u8 = undefined;
    const segment = try sanitize("COM1.txt", &buffer);
    try testing.expect(!segment.verbatim);
    try testing.expectEqualStrings("_COM1.txt", buffer[0..segment.len]);
}

test "sanitizeSegment drops the tail the underscore displaces" {
    var buffer: [5]u8 = undefined;
    const segment = try sanitize("con.x", &buffer);
    try testing.expectEqualStrings("_con", buffer[0..segment.len]);
}

test "sanitizeSegment refuses a capacity below the minimum" {
    var buffer: [1]u8 = undefined;
    try testing.expectError(error.CapTooSmall, sanitize("a", &buffer));
}

test "sanitizeSegment output can never escape a parent" {
    var buffer: [16]u8 = undefined;
    for ([_][]const u8{ "../../etc/passwd", "..", "/", "" }) |raw| {
        const segment = try sanitize(raw, &buffer);
        const produced = buffer[0..segment.len];
        try testing.expect(!path.dotSegment(produced));
        try testing.expect(!path.hasSeparator(produced));
    }
}

test "joinUnder composes parent and segment" {
    var buffer: [32]u8 = undefined;
    const len = try path.joinUnder("/books/incoming", "a.txt", &buffer);
    try testing.expectEqualStrings("/books/incoming/a.txt", buffer[0..len]);
}

test "joinUnder refuses a segment that is not one level" {
    var buffer: [32]u8 = undefined;
    for ([_][]const u8{ "", ".", "..", "a/b", "/etc/passwd" }) |seg| {
        try testing.expectError(error.NotOneSegment, path.joinUnder("/p", seg, &buffer));
    }
}

test "joinUnder refuses to truncate" {
    var buffer: [8]u8 = undefined;
    try testing.expectError(error.WouldTruncate, path.joinUnder("/parent", "a.txt", &buffer));
}

test "joinUnder fits a path that needs exactly the buffer" {
    var buffer: [5]u8 = undefined;
    const len = try path.joinUnder("/p", "a", &buffer);
    try testing.expectEqualStrings("/p/a", buffer[0..len]);
}

test "contained treats the directory boundary as significant" {
    try testing.expect(try path.contained("/a/b", "/a/b"));
    try testing.expect(try path.contained("/a/b", "/a/b/c"));
    try testing.expect(!try path.contained("/a/b", "/a/bb"));
    try testing.expect(!try path.contained("/a/b", "/a"));
    try testing.expect(!try path.contained("/a/b", "/x/a/b"));
}

test "contained ignores trailing slashes on the parent" {
    try testing.expect(try path.contained("/a/b///", "/a/b/c"));
    try testing.expectEqualStrings("/a/b", path.trimmedParent("/a/b///"));
}

test "contained refuses a parent that is empty or only slashes" {
    try testing.expectError(error.EmptyParent, path.contained("", "/a"));
    try testing.expectError(error.EmptyParent, path.contained("///", "/a"));
}

test "a sanitized segment joined under a parent stays contained" {
    var segment_buffer: [32]u8 = undefined;
    var joined: [64]u8 = undefined;
    const segment = try sanitize("../../etc/passwd", &segment_buffer);
    const len = try path.joinUnder("/books", segment_buffer[0..segment.len], &joined);
    try testing.expect(try path.contained("/books", joined[0..len]));
}
