//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Sites against records: a site the table names passes, a site it misses
//! fails, and a record with no site behind it fails too.

const std = @import("std");
const testing = std.testing;

const checker = @import("checker");
const arm = checker.arm;
const report = checker.report;
const fixture = @import("elf_fixture.zig");
const Address = fixture.Address;
const Symbol = fixture.Symbol;

comptime {
    _ = @import("elf_fixture.zig");
}

const table_sites = [_]fixture.Reloc{
    .{ .at = 0x10000020, .symbol = Symbol.double, .kind = arm.abs32 },
    .{ .at = 0x10000024, .symbol = Symbol.square, .kind = arm.abs32 },
};
const table_words = [4]u32{ 0, 5, Address.double, Address.square };

fn run(spec: fixture.Spec, buf: []u8) !struct { usize, []const u8 } {
    const image = fixture.build(spec);
    var writer: std.Io.Writer = .fixed(buf);
    const wrong = try report.write(&writer, "m.elf", image.bytes());
    return .{ wrong, writer.buffered() };
}

test "two sites, both in the records: a pass that says how many" {
    var buf: [1024]u8 = undefined;
    const wrong, const out = try run(.{
        .data = &table_sites,
        .words = table_words,
        .records = &.{ 0x10000020, 0x10000024 },
    }, &buf);
    try testing.expectEqual(@as(usize, 0), wrong);
    try testing.expectEqualStrings(
        "m.elf: OK, 2 word(s) of data hold an address, each one in the rebase records\n",
        out,
    );
}

test "two sites, one left out of the records: a failure naming that one" {
    var buf: [1024]u8 = undefined;
    const wrong, const out = try run(.{
        .data = &table_sites,
        .words = table_words,
        .records = &.{0x10000024},
    }, &buf);
    try testing.expectEqual(@as(usize, 1), wrong);
    try testing.expectEqualStrings(
        \\m.elf: .data at 0x10000020: R_ARM_ABS32 -> double (0x00030191)
        \\m.elf: FAIL, 1 word(s) of data hold an address nothing rebases at load
        \\
    , out);
}

test "sites and an empty table: every one is still a failure" {
    var buf: [1024]u8 = undefined;
    const spec: fixture.Spec = .{ .data = &table_sites, .words = table_words, .records = &.{} };
    const wrong, _ = try run(spec, &buf);
    try testing.expectEqual(@as(usize, 2), wrong);
}

test "a record one byte off its site covers nothing" {
    var buf: [1024]u8 = undefined;
    const wrong, const out = try run(.{
        .data = &table_sites,
        .words = table_words,
        .records = &.{ 0x10000021, 0x10000024 },
    }, &buf);
    // The site it missed, and the record that names no site.
    try testing.expectEqual(@as(usize, 2), wrong);
    try testing.expect(std.mem.indexOf(u8, out, ".data at 0x10000020: R_ARM_ABS32") != null);
    try testing.expect(std.mem.indexOf(u8, out, "rebase record 0x10000021: no data word") != null);
}

test "a record with no site behind it fails: it would change an ordinary word" {
    var buf: [1024]u8 = undefined;
    const wrong, const out = try run(.{
        .data = &table_sites,
        .words = table_words,
        .records = &.{ 0x1000001c, 0x10000020, 0x10000024 },
    }, &buf);
    try testing.expectEqual(@as(usize, 1), wrong);
    try testing.expectEqualStrings(
        \\m.elf: rebase record 0x1000001c: no data word at that address holds an address
        \\m.elf: FAIL, 1 rebase record(s) would change a word they should not
        \\
    , out);
}

test "a record listed twice fails: its word would be rebased twice" {
    var buf: [1024]u8 = undefined;
    const wrong, const out = try run(.{
        .data = &table_sites,
        .words = table_words,
        .records = &.{ 0x10000020, 0x10000024, 0x10000020 },
    }, &buf);
    try testing.expectEqual(@as(usize, 1), wrong);
    try testing.expect(std.mem.indexOf(u8, out, "0x10000020: listed more than once") != null);
}

test "a record cannot cover a site that is not a whole word" {
    var buf: [1024]u8 = undefined;
    const wrong, _ = try run(.{
        .words = table_words,
        .data = &.{.{ .at = 0x10000020, .symbol = Symbol.double, .kind = arm.abs16 }},
        .records = &.{0x10000020},
    }, &buf);
    // The site is uncovered, and the record names no rebasable word.
    try testing.expectEqual(@as(usize, 2), wrong);
}

test "an empty table and nothing to rebase is still the clean pass" {
    var buf: [1024]u8 = undefined;
    const wrong, const out = try run(.{ .records = &.{} }, &buf);
    try testing.expectEqual(@as(usize, 0), wrong);
    try testing.expectEqualStrings("m.elf: OK, no absolute relocation in a data section\n", out);
}

test "sites only in a debug section need no records" {
    var buf: [1024]u8 = undefined;
    const wrong, _ = try run(.{ .debug = &.{
        .{ .at = 4, .symbol = Symbol.double, .kind = arm.abs32 },
    } }, &buf);
    try testing.expectEqual(@as(usize, 0), wrong);
}
