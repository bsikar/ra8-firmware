//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Which SSIE word pair each PCM bit depth maps to.

const std = @import("std");
const audio_word = @import("audio_word");

const ok: u32 = 0;
const invalid_arg: u32 = 0x103;

test "every native DA7212 depth maps" {
    try std.testing.expectEqual(@as(u32, 1), audio_word.forBits(16).?.data);
    try std.testing.expectEqual(@as(u32, 5), audio_word.forBits(24).?.data);
    try std.testing.expectEqual(@as(u32, 6), audio_word.forBits(32).?.data);
}

test "8-bit frames use the 8-bit slot" {
    const w = audio_word.forBits(8).?;
    try std.testing.expectEqual(@as(u32, 0), w.data);
    try std.testing.expectEqual(@as(u32, 0), w.system);
}

test "16-bit frames use the 16-bit slot" {
    const w = audio_word.forBits(16).?;
    try std.testing.expectEqual(@as(u32, 1), w.data);
    try std.testing.expectEqual(@as(u32, 1), w.system);
}

test "18, 20 and 22 bits all ride in a 24-bit slot" {
    try std.testing.expectEqual(@as(u32, 2), audio_word.forBits(18).?.system);
    try std.testing.expectEqual(@as(u32, 2), audio_word.forBits(20).?.system);
    try std.testing.expectEqual(@as(u32, 2), audio_word.forBits(22).?.system);
}

test "those three keep their own data widths" {
    try std.testing.expectEqual(@as(u32, 2), audio_word.forBits(18).?.data);
    try std.testing.expectEqual(@as(u32, 3), audio_word.forBits(20).?.data);
    try std.testing.expectEqual(@as(u32, 4), audio_word.forBits(22).?.data);
}

test "32-bit frames use the 32-bit slot" {
    const w = audio_word.forBits(32).?;
    try std.testing.expectEqual(@as(u32, 6), w.data);
    try std.testing.expectEqual(@as(u32, 3), w.system);
}

test "the data word never exceeds its system slot" {
    const depths = [_]u8{ 8, 16, 18, 20, 22, 24, 32 };
    const slot_bits = [_]u16{ 8, 16, 24, 32 };
    const data_bits = [_]u16{ 8, 16, 18, 20, 22, 24, 32 };
    for (depths, data_bits) |depth, bits| {
        const w = audio_word.forBits(depth).?;
        try std.testing.expect(bits <= slot_bits[w.system]);
    }
}

test "a depth the SSIE has no code for is refused" {
    try std.testing.expectEqual(@as(?audio_word.Words, null), audio_word.forBits(0));
    try std.testing.expectEqual(@as(?audio_word.Words, null), audio_word.forBits(12));
    try std.testing.expectEqual(@as(?audio_word.Words, null), audio_word.forBits(17));
    try std.testing.expectEqual(@as(?audio_word.Words, null), audio_word.forBits(255));
}

test "the out-parameter form agrees with the optional form" {
    var w: audio_word.Words = undefined;
    try std.testing.expectEqual(ok, audio_word.resolve(24, &w));
    try std.testing.expectEqualDeep(audio_word.forBits(24).?, w);
}

test "the out-parameter form refuses an unknown depth" {
    var w: audio_word.Words = undefined;
    try std.testing.expectEqual(invalid_arg, audio_word.resolve(17, &w));
}

test "a refused depth leaves the output alone" {
    var w = audio_word.Words{ .data = 0xAA, .system = 0xBB };
    _ = audio_word.resolve(17, &w);
    try std.testing.expectEqual(@as(u32, 0xAA), w.data);
}
