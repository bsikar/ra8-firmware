//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Reading a module's rebase records back out of the linked image.

const std = @import("std");
const testing = std.testing;

const checker = @import("checker");
const Records = checker.records.Records;
const fixture = @import("elf_fixture.zig");
const Address = fixture.Address;

comptime {
    _ = @import("elf_fixture.zig");
}

fn read(spec: fixture.Spec, image: *fixture.Image) !Records {
    image.* = fixture.build(spec);
    return Records.read(try checker.elf32.File.init(image.bytes()));
}

test "a module with no table symbols has no records" {
    var image: fixture.Image = undefined;
    const table = try read(.{}, &image);
    try testing.expectEqual(@as(usize, 0), table.count());
    try testing.expect(!table.has(Address.data));
}

test "a table that is there and empty has no records either" {
    var image: fixture.Image = undefined;
    const table = try read(.{ .records = &.{} }, &image);
    try testing.expectEqual(@as(usize, 0), table.count());
}

test "the records are the words between the two symbols, in order" {
    var image: fixture.Image = undefined;
    const table = try read(.{ .records = &.{ 0x10000020, 0x10000024, 0x1000001c } }, &image);
    try testing.expectEqual(@as(usize, 3), table.count());
    try testing.expectEqual(@as(u32, 0x10000020), table.at(0));
    try testing.expectEqual(@as(u32, 0x10000024), table.at(1));
    try testing.expectEqual(@as(u32, 0x1000001c), table.at(2));
}

test "an address is covered only when a record names exactly it" {
    var image: fixture.Image = undefined;
    const table = try read(.{ .records = &.{ 0x10000020, 0x10000024 } }, &image);
    try testing.expect(table.has(0x10000020));
    try testing.expect(table.has(0x10000024));
    // One byte off, the word before, and the word after.
    try testing.expect(!table.has(0x10000021));
    try testing.expect(!table.has(0x1000001c));
    try testing.expect(!table.has(0x10000028));
}

test "a table with a start and no end is refused, not read as empty" {
    var image: fixture.Image = undefined;
    const spec: fixture.Spec = .{ .records = &.{0x10000020}, .without_records_end = true };
    try testing.expectError(error.BadRecords, read(spec, &image));
}
