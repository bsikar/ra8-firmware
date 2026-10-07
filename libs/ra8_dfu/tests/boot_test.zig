//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Zig-native cover of the pure boot logic, carrying the same MC/DC vectors
//! `tests/misc/src/test_ra8_dfu_boot.c` drives through the C ABI. Both run:
//! this suite through `zig build test`, the C one through `zig build test-c`
//! against this archive.

const std = @import("std");

const crc32 = @import("crc32");
const image = @import("image");
const slot = @import("slot");

const layout = image.layout;

/// A header with the fields a case varies; entry fixed at the run base.
fn mkHeader(magic: u32, seq: u32, len: u32, crc: u32) image.Header {
    return .{
        .magic = magic,
        .seq = seq,
        .img_len = len,
        .img_crc32 = crc,
        .entry = layout.run_base,
        .rsv0 = 0,
        .rsv1 = 0,
        .rsv2 = 0,
    };
}

const len_ok: u32 = 0x0000_4000;
const len_unaligned: u32 = 0x0000_0021;
const len_too_big: u32 = layout.img_max + layout.page_size;
const crc_good: u32 = 0x1234_5678;
const crc_bad: u32 = 0x8765_4321;

test "crc32 published check values" {
    try std.testing.expectEqual(@as(u32, 0x0000_0000), crc32.compute(""));
    try std.testing.expectEqual(@as(u32, 0xCBF4_3926), crc32.compute("123456789"));
    try std.testing.expectEqual(@as(u32, 0xE8B7_BE43), crc32.compute("a"));
    try std.testing.expectEqual(@as(u32, 0xD202_EF8D), crc32.compute(&.{0x00}));
}

test "headerValid: outer magic/len/crc vectors" {
    const v1 = mkHeader(layout.hdr_magic, 1, len_ok, crc_good);
    try std.testing.expect(image.headerValid(&v1, crc_good)); // T T T

    const v2 = mkHeader(layout.hdr_magic + 1, 1, len_ok, crc_good);
    try std.testing.expect(!image.headerValid(&v2, crc_good)); // F T T

    const v3 = mkHeader(layout.hdr_magic, 1, 0, crc_good);
    try std.testing.expect(!image.headerValid(&v3, crc_good)); // T F T

    try std.testing.expect(!image.headerValid(&v1, crc_bad)); // T T F
}

test "headerValid: inner length vectors" {
    const aligned = mkHeader(layout.hdr_magic, 1, len_ok, crc_good);
    try std.testing.expect(image.headerValid(&aligned, crc_good));

    const empty = mkHeader(layout.hdr_magic, 1, 0, crc_good);
    try std.testing.expect(!image.headerValid(&empty, crc_good));

    const too_big = mkHeader(layout.hdr_magic, 1, len_too_big, crc_good);
    try std.testing.expect(!image.headerValid(&too_big, crc_good));

    const unaligned = mkHeader(layout.hdr_magic, 1, len_unaligned, crc_good);
    try std.testing.expect(!image.headerValid(&unaligned, crc_good));
}

test "runTargetValid: entry and length" {
    const bad_entry: u32 = 0x0202_0000; // slot A base: a real address, not the run base

    try std.testing.expect(image.runTargetValid(layout.run_base, len_ok));
    try std.testing.expect(!image.runTargetValid(bad_entry, len_ok));
    try std.testing.expect(!image.runTargetValid(layout.run_base, 0));
    try std.testing.expect(!image.runTargetValid(layout.run_base, len_too_big));
    try std.testing.expect(!image.runTargetValid(layout.run_base, len_unaligned));
    // Exactly img_max is page-aligned and valid.
    try std.testing.expect(image.runTargetValid(layout.run_base, layout.img_max));
}

fn candidate(valid: bool, seq: u32) slot.Candidate {
    return .{ .valid = valid, .seq = seq };
}

test "select: none-decision vectors" {
    try std.testing.expectEqual(slot.Slot.none, slot.select(candidate(false, 0), candidate(false, 0)));
    try std.testing.expectEqual(slot.Slot.a, slot.select(candidate(true, 5), candidate(false, 0)));
    try std.testing.expectEqual(slot.Slot.b, slot.select(candidate(false, 0), candidate(true, 5)));
}

test "select: slot-A-decision vectors" {
    try std.testing.expectEqual(slot.Slot.a, slot.select(candidate(true, 9), candidate(true, 3)));
    try std.testing.expectEqual(slot.Slot.b, slot.select(candidate(true, 3), candidate(true, 9)));
    try std.testing.expectEqual(slot.Slot.a, slot.select(candidate(true, 4), candidate(true, 4)));
    try std.testing.expectEqual(slot.Slot.a, slot.select(candidate(true, 1), candidate(false, 9)));
}

test "decide: trigger wins, otherwise the selection maps" {
    try std.testing.expectEqual(slot.Action.dfu, slot.decide(true, candidate(true, 5), candidate(false, 0)));
    try std.testing.expectEqual(slot.Action.jump_a, slot.decide(false, candidate(true, 5), candidate(false, 0)));
    try std.testing.expectEqual(slot.Action.jump_b, slot.decide(false, candidate(false, 0), candidate(true, 5)));
    try std.testing.expectEqual(slot.Action.jump_b, slot.decide(false, candidate(true, 3), candidate(true, 9)));
    try std.testing.expectEqual(slot.Action.jump_a, slot.decide(false, candidate(true, 9), candidate(true, 3)));
    try std.testing.expectEqual(slot.Action.dfu, slot.decide(false, candidate(false, 0), candidate(false, 0)));
}

test "the C enum values the ABI returns" {
    try std.testing.expectEqual(@as(u8, 0), @backingInt(slot.Slot.a));
    try std.testing.expectEqual(@as(u8, 1), @backingInt(slot.Slot.b));
    try std.testing.expectEqual(@as(u8, 2), @backingInt(slot.Slot.none));
    try std.testing.expectEqual(@as(u8, 0), @backingInt(slot.Action.dfu));
    try std.testing.expectEqual(@as(u8, 1), @backingInt(slot.Action.jump_a));
    try std.testing.expectEqual(@as(u8, 2), @backingInt(slot.Action.jump_b));
}
