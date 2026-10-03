//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Which link range an address is in: the choice between the code delta and
//! the data delta, made at build time the way the start-up makes it.

const std = @import("std");
const testing = std.testing;

const checker = @import("checker");
const layout = checker.layout;
const fixture = @import("elf_fixture.zig");
const Address = fixture.Address;

comptime {
    _ = @import("elf_fixture.zig");
}

fn read(spec: fixture.Spec, image: *fixture.Image) !?layout.Layout {
    image.* = fixture.build(spec);
    return layout.Layout.read(try checker.elf32.File.init(image.bytes()));
}

test "the two ranges are the linker script's four symbols" {
    var image: fixture.Image = undefined;
    const ranges = (try read(.{}, &image)).?;
    try testing.expectEqual(Address.code_start, ranges.code.start);
    try testing.expectEqual(Address.code_end, ranges.code.end);
    try testing.expectEqual(Address.data_start, ranges.data.start);
    try testing.expectEqual(Address.data_end, ranges.data.end);
}

test "an image that does not name its ranges is not a module" {
    var image: fixture.Image = undefined;
    try testing.expectEqual(null, try read(.{ .without_ranges = true }, &image));
}

test "a code address is code, with or without the Thumb bit" {
    var image: fixture.Image = undefined;
    const ranges = (try read(.{}, &image)).?;
    try testing.expectEqual(layout.Region.code, ranges.region(Address.code_start));
    try testing.expectEqual(layout.Region.code, ranges.region(Address.double));
    try testing.expectEqual(layout.Region.code, ranges.region(Address.double & ~@as(u32, 1)));
    try testing.expectEqual(layout.Region.code, ranges.region(Address.code_end - 1));
}

test "a data address is data" {
    var image: fixture.Image = undefined;
    const ranges = (try read(.{}, &image)).?;
    try testing.expectEqual(layout.Region.data, ranges.region(Address.data_start));
    try testing.expectEqual(layout.Region.data, ranges.region(Address.counter));
    try testing.expectEqual(layout.Region.data, ranges.region(Address.data_end - 1));
}

test "anything else is outside, and each range ends before its end address" {
    var image: fixture.Image = undefined;
    const ranges = (try read(.{}, &image)).?;
    const outside = [_]u32{
        0,                      Address.code_start - 1, Address.code_end,
        Address.data_start - 1, Address.data_end,       0xFFFFFFFF,
    };
    for (outside) |address| {
        try testing.expectEqual(layout.Region.outside, ranges.region(address));
    }
}
