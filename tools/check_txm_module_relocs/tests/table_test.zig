//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Building the rebase table from a first link: what goes in, what the
//! build refuses, and the assembly that comes out.

const std = @import("std");
const testing = std.testing;

const checker = @import("checker");
const arm = checker.arm;
const table = checker.table;
const fixture = @import("elf_fixture.zig");
const Address = fixture.Address;
const Symbol = fixture.Symbol;

comptime {
    _ = @import("elf_fixture.zig");
}

/// The two table words of the RA8FW-534 probe: `.data` words two and three
/// holding the addresses of two functions.
const table_sites = [_]fixture.Reloc{
    .{ .at = 0x10000020, .symbol = Symbol.double, .kind = arm.abs32 },
    .{ .at = 0x10000024, .symbol = Symbol.square, .kind = arm.abs32 },
};
const table_words = [4]u32{ 0, 5, Address.double, Address.square };

fn sites(spec: fixture.Spec, out: []u32, problem: *?table.Problem) ![]u32 {
    const image = fixture.build(spec);
    return table.sites(image.bytes(), out, problem);
}

test "each data word that holds an address becomes one entry: its own address" {
    var out: [8]u32 = undefined;
    var problem: ?table.Problem = null;
    const got = try sites(.{ .data = &table_sites, .words = table_words }, &out, &problem);
    try testing.expectEqualSlices(u32, &.{ 0x10000020, 0x10000024 }, got);
    try testing.expectEqual(null, problem);
}

test "the entries come out in address order whatever order the linker kept" {
    var out: [8]u32 = undefined;
    var problem: ?table.Problem = null;
    const got = try sites(.{ .words = table_words, .data = &.{
        table_sites[1],
        table_sites[0],
    } }, &out, &problem);
    try testing.expectEqualSlices(u32, &.{ 0x10000020, 0x10000024 }, got);
}

test "a word holding a data address is an entry too" {
    var out: [8]u32 = undefined;
    var problem: ?table.Problem = null;
    const got = try sites(.{
        .words = .{ Address.counter, 0, 0, 0 },
        .data = &.{.{ .at = Address.data, .symbol = Symbol.counter, .kind = arm.abs32 }},
    }, &out, &problem);
    try testing.expectEqualSlices(u32, &.{Address.data}, got);
}

test "a module with nothing to rebase has an empty table" {
    var out: [8]u32 = undefined;
    var problem: ?table.Problem = null;
    try testing.expectEqual(@as(usize, 0), (try sites(.{}, &out, &problem)).len);
}

test "a site in the code region is refused: code is never written" {
    var out: [8]u32 = undefined;
    var problem: ?table.Problem = null;
    const spec: fixture.Spec = .{ .rodata = &.{
        .{ .at = Address.rodata, .symbol = Symbol.double, .kind = arm.abs32 },
    } };
    try testing.expectError(error.SiteInCode, sites(spec, &out, &problem));
    try testing.expectEqual(Address.rodata, problem.?.site.address);
    try testing.expectEqualStrings(".rodata", problem.?.site.section);
}

test "a stored value in neither link range is refused" {
    var out: [8]u32 = undefined;
    var problem: ?table.Problem = null;
    for ([_]u32{ 0, 0x00040000, 0x10008000, 0x50000000 }) |stored| {
        const spec: fixture.Spec = .{ .data = &table_sites, .words = .{ 0, 5, stored, 0x30195 } };
        try testing.expectError(error.ValueOutsideModule, sites(spec, &out, &problem));
        try testing.expectEqual(@as(u32, 0x10000020), problem.?.site.address);
        try testing.expectEqual(stored, problem.?.stored);
    }
}

test "a relocation that is not a whole word is refused" {
    var out: [8]u32 = undefined;
    var problem: ?table.Problem = null;
    const spec: fixture.Spec = .{ .words = table_words, .data = &.{
        .{ .at = 0x10000020, .symbol = Symbol.double, .kind = arm.abs16 },
    } };
    try testing.expectError(error.NotAWholeWord, sites(spec, &out, &problem));
    try testing.expectEqual(arm.abs16, problem.?.site.kind);
}

test "any dynamic relocation is refused" {
    var out: [8]u32 = undefined;
    var problem: ?table.Problem = null;
    const relative = 23;
    const spec: fixture.Spec = .{ .dynamic = &.{
        .{ .at = 0x10000020, .symbol = 0, .kind = relative },
    } };
    try testing.expectError(error.DynamicRelocation, sites(spec, &out, &problem));
}

test "an image that does not name its link ranges is refused" {
    var out: [8]u32 = undefined;
    var problem: ?table.Problem = null;
    const spec: fixture.Spec = .{ .without_ranges = true, .data = &table_sites };
    try testing.expectError(error.NotAModule, sites(spec, &out, &problem));
}

test "more sites than there is room for is an error, not a shorter table" {
    var out: [1]u32 = undefined;
    var problem: ?table.Problem = null;
    const spec: fixture.Spec = .{ .data = &table_sites, .words = table_words };
    try testing.expectError(error.TooManySites, sites(spec, &out, &problem));
}

test "the table is one word for each entry between its two symbols" {
    var buf: [1024]u8 = undefined;
    var stream = std.io.fixedBufferStream(&buf);
    try table.write(stream.writer(), &.{ 0x10000020, 0x10000024 }, null);
    const text = stream.getWritten();
    const section = ".section .rodata.txm_rebase,\"a\",%progbits";
    try testing.expect(std.mem.indexOf(u8, text, section) != null);
    try testing.expect(std.mem.endsWith(u8, text,
        \\__txm_rebase_start__:
        \\    .word 0x10000020
        \\    .word 0x10000024
        \\__txm_rebase_end__:
        \\
    ));
}

test "an empty table is the two symbols at one address" {
    var buf: [1024]u8 = undefined;
    var stream = std.io.fixedBufferStream(&buf);
    try table.write(stream.writer(), &.{}, null);
    const text = stream.getWritten();
    try testing.expect(std.mem.endsWith(u8, text, "__txm_rebase_start__:\n__txm_rebase_end__:\n"));
    try testing.expect(std.mem.indexOf(u8, text, ".word") == null);
}

test "leaving one entry out drops exactly that one" {
    var buf: [1024]u8 = undefined;
    var stream = std.io.fixedBufferStream(&buf);
    try table.write(stream.writer(), &.{ 0x10000020, 0x10000024 }, 0);
    const text = stream.getWritten();
    try testing.expect(std.mem.indexOf(u8, text, "0x10000020") == null);
    try testing.expect(std.mem.indexOf(u8, text, "    .word 0x10000024\n") != null);
}
