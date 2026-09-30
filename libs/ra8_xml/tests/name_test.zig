//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const name = @import("name");

test "a Name opens with a letter, an underscore or a colon" {
    for ("AZaz_:") |byte| try std.testing.expect(name.isStart(byte));
    for ("09.- \t<&/") |byte| try std.testing.expect(!name.isStart(byte));
}

test "a Name continues with digits, dots and hyphens as well" {
    for ("AZaz_:09.-") |byte| try std.testing.expect(name.isChar(byte));
    for (" \t<&/\"") |byte| try std.testing.expect(!name.isChar(byte));
}

test "legal names are accepted" {
    const legal = [_][]const u8{ "a", "_", ":", "item", "ns:tag", "x-1", "a.b.c", "_private9" };
    for (legal) |text| try std.testing.expect(name.isValid(text));
}

test "illegal names are rejected" {
    const illegal = [_][]const u8{ "", "1tag", "-tag", ".tag", "has space", "has<angle", "a/b" };
    for (illegal) |text| try std.testing.expect(!name.isValid(text));
}

test "the frame cap leaves room for the terminator" {
    try std.testing.expectEqual(@as(usize, 64), name.limits.name_cap);
    try std.testing.expectEqual(name.limits.name_cap - 1, name.limits.name_bytes);
}
