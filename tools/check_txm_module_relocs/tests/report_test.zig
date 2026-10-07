//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The report: the lines a person reads, and the count the exit status
//! comes from.

const std = @import("std");
const testing = std.testing;

const checker = @import("checker");
const arm = checker.arm;
const report = checker.report;
const fixture = @import("elf_fixture.zig");
const Symbol = fixture.Symbol;

comptime {
    _ = @import("elf_fixture.zig");
}

fn text(spec: fixture.Spec, buf: []u8) !struct { usize, []const u8 } {
    const image = fixture.build(spec);
    var writer: std.Io.Writer = .fixed(buf);
    const count = try report.write(&writer, "m.elf", image.bytes());
    return .{ count, writer.buffered() };
}

test "a clean image is one line saying so, and no findings" {
    var buf: [512]u8 = undefined;
    const count, const out = try text(.{}, &buf);
    try testing.expectEqual(@as(usize, 0), count);
    try testing.expectEqualStrings("m.elf: OK, no absolute relocation in a data section\n", out);
}

test "each finding is a line naming the section, the site, the type and the symbol" {
    var buf: [512]u8 = undefined;
    const count, const out = try text(.{ .data = &.{
        .{ .at = 0x10000020, .symbol = Symbol.double, .kind = arm.abs32 },
        .{ .at = 0x10000024, .symbol = Symbol.square, .kind = arm.abs32 },
    } }, &buf);
    try testing.expectEqual(@as(usize, 2), count);
    try testing.expectEqualStrings(
        \\m.elf: .data at 0x10000020: R_ARM_ABS32 -> double (0x00030191)
        \\m.elf: .data at 0x10000024: R_ARM_ABS32 -> square (0x00030195)
        \\m.elf: FAIL, 2 word(s) of data hold an address nothing rebases at load
        \\
    , out);
}

test "a type the name table does not carry is shown by its number" {
    var buf: [512]u8 = undefined;
    const count, const out = try text(.{ .data = &.{
        .{ .at = 0x10000020, .symbol = Symbol.counter, .kind = 200 },
    } }, &buf);
    try testing.expectEqual(@as(usize, 1), count);
    try testing.expect(std.mem.indexOf(u8, out, "relocation type 200 -> counter") != null);
}

test "an image that cannot be read is an error, not a clean report" {
    var buf: [512]u8 = undefined;
    var writer: std.Io.Writer = .fixed(&buf);
    try testing.expectError(error.NotElf, report.write(&writer, "m.elf", "nonsense"));
    try testing.expectEqual(@as(usize, 0), writer.buffered().len);
}

test "the name table and the allow-list agree on what they know" {
    try testing.expectEqualStrings("R_ARM_ABS32", arm.name(arm.abs32).?);
    try testing.expectEqualStrings("R_ARM_GOT_BREL", arm.name(arm.got_brel).?);
    try testing.expectEqual(null, arm.name(200));
    try testing.expect(!arm.movesWithTheModule(arm.abs32));
    try testing.expect(arm.movesWithTheModule(arm.got_brel));
    try testing.expect(!arm.movesWithTheModule(200));
}
