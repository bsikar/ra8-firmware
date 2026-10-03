//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The rule itself: which relocations in which sections are findings.

const std = @import("std");
const testing = std.testing;

const checker = @import("checker");
const arm = checker.arm;
const check = checker.check;
const fixture = @import("elf_fixture.zig");
const Symbol = fixture.Symbol;

comptime {
    _ = @import("elf_fixture.zig");
}

/// Every finding in the image `spec` describes.
fn findings(spec: fixture.Spec, out: []check.Finding, image: *fixture.Image) ![]check.Finding {
    image.* = fixture.build(spec);
    var walk = try check.Iterator.init(image.bytes());
    var count: usize = 0;
    while (try walk.next()) |finding| : (count += 1) out[count] = finding;
    return out[0..count];
}

test "an image with no relocations in data is clean" {
    var image: fixture.Image = undefined;
    var out: [8]check.Finding = undefined;
    try testing.expectEqual(@as(usize, 0), (try findings(.{}, &out, &image)).len);
}

test "a data word holding an absolute address is a finding, with its site and symbol" {
    var image: fixture.Image = undefined;
    var out: [8]check.Finding = undefined;
    const found = try findings(.{ .data = &.{
        .{ .at = 0x10000020, .symbol = Symbol.double, .kind = arm.abs32 },
        .{ .at = 0x10000024, .symbol = Symbol.square, .kind = arm.abs32 },
    } }, &out, &image);

    try testing.expectEqual(@as(usize, 2), found.len);
    try testing.expectEqualStrings(".data", found[0].section);
    try testing.expectEqual(@as(u32, 0x10000020), found[0].address);
    try testing.expectEqual(arm.abs32, found[0].kind);
    try testing.expectEqualStrings("double", found[0].symbol);
    try testing.expectEqual(@as(u32, 0x30191), found[0].value);
    try testing.expectEqual(@as(u32, 0x10000024), found[1].address);
    try testing.expectEqualStrings("square", found[1].symbol);
}

test "an absolute relocation only in a debug section is ignored" {
    var image: fixture.Image = undefined;
    var out: [8]check.Finding = undefined;
    const found = try findings(.{ .debug = &.{
        .{ .at = 4, .symbol = Symbol.double, .kind = arm.abs32 },
        .{ .at = 8, .symbol = Symbol.counter, .kind = arm.abs32 },
    } }, &out, &image);
    try testing.expectEqual(@as(usize, 0), found.len);
}

test "read-only data is data: an absolute address there is not rebased either" {
    var image: fixture.Image = undefined;
    var out: [8]check.Finding = undefined;
    const found = try findings(.{ .rodata = &.{
        .{ .at = 0x30300, .symbol = Symbol.double, .kind = arm.abs32 },
    } }, &out, &image);
    try testing.expectEqual(@as(usize, 1), found.len);
    try testing.expectEqualStrings(".rodata", found[0].section);
}

test "code and the GOT are left alone" {
    var image: fixture.Image = undefined;
    var out: [8]check.Finding = undefined;
    const abs = [_]fixture.Reloc{.{ .at = 0x30090, .symbol = Symbol.counter, .kind = arm.abs32 }};
    try testing.expectEqual(@as(usize, 0), (try findings(.{ .text = &abs }, &out, &image)).len);
    try testing.expectEqual(@as(usize, 0), (try findings(.{ .got = &abs }, &out, &image)).len);
}

test "a relocation that moves with the module is not a finding" {
    var image: fixture.Image = undefined;
    var out: [8]check.Finding = undefined;
    const kinds = [_]arm.Kind{
        arm.none,      arm.rel32,    arm.sbrel32, arm.gotoff32,
        arm.base_prel, arm.got_brel, arm.prel31,  arm.got_prel,
    };
    for (kinds) |kind| {
        const one = [_]fixture.Reloc{.{ .at = 0x10000020, .symbol = Symbol.double, .kind = kind }};
        try testing.expectEqual(@as(usize, 0), (try findings(.{ .data = &one }, &out, &image)).len);
    }
}

test "every other type fails closed, known to the table or not" {
    var image: fixture.Image = undefined;
    var out: [8]check.Finding = undefined;
    // Absolute types of other widths, the ABS32 alias, and two numbers the
    // name table has never heard of.
    const kinds = [_]arm.Kind{ arm.abs16, arm.abs12, arm.abs8, arm.target1, 1, 200 };
    for (kinds) |kind| {
        const one = [_]fixture.Reloc{.{ .at = 0x10000020, .symbol = Symbol.double, .kind = kind }};
        const found = try findings(.{ .data = &one }, &out, &image);
        try testing.expectEqual(@as(usize, 1), found.len);
        try testing.expectEqual(kind, found[0].kind);
    }
}

test "a relocation against a section is named after the section" {
    var image: fixture.Image = undefined;
    var out: [8]check.Finding = undefined;
    const found = try findings(.{ .data = &.{
        .{ .at = 0x10000020, .symbol = Symbol.data_section, .kind = arm.abs32 },
    } }, &out, &image);
    try testing.expectEqualStrings(".data", found[0].symbol);
    try testing.expectEqual(@as(u32, 0x10000018), found[0].value);
}

test "findings come from every data section, in file order, past clean ones" {
    var image: fixture.Image = undefined;
    var out: [8]check.Finding = undefined;
    const found = try findings(.{
        .text = &.{.{ .at = 0x30090, .symbol = Symbol.counter, .kind = arm.abs32 }},
        .rodata = &.{.{ .at = 0x30300, .symbol = Symbol.double, .kind = arm.abs32 }},
        .data = &.{
            .{ .at = 0x10000018, .symbol = Symbol.double, .kind = arm.rel32 },
            .{ .at = 0x10000020, .symbol = Symbol.square, .kind = arm.abs32 },
        },
        .debug = &.{.{ .at = 0, .symbol = Symbol.double, .kind = arm.abs32 }},
    }, &out, &image);
    try testing.expectEqual(@as(usize, 2), found.len);
    try testing.expectEqualStrings(".rodata", found[0].section);
    try testing.expectEqualStrings(".data", found[1].section);
    try testing.expectEqualStrings("square", found[1].symbol);
}

test "relocations stored with addends are read the same way" {
    var image: fixture.Image = undefined;
    var out: [8]check.Finding = undefined;
    const found = try findings(.{ .with_addends = true, .data = &.{
        .{ .at = 0x10000020, .symbol = Symbol.double, .kind = arm.rel32 },
        .{ .at = 0x10000024, .symbol = Symbol.square, .kind = arm.abs32 },
    } }, &out, &image);
    try testing.expectEqual(@as(usize, 1), found.len);
    try testing.expectEqual(@as(u32, 0x10000024), found[0].address);
    try testing.expectEqualStrings("square", found[0].symbol);
}

test "a relocation naming a symbol that is not there is an error" {
    var image: fixture.Image = undefined;
    var out: [8]check.Finding = undefined;
    const bad = [_]fixture.Reloc{.{ .at = 0x10000020, .symbol = 99, .kind = arm.abs32 }};
    try testing.expectError(error.Truncated, findings(.{ .data = &bad }, &out, &image));
}
