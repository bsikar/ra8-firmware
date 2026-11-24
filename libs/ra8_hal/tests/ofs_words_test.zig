//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const ofs_words = @import("ofs_words");

test "the OFS quartet inventory matches ra8_ofs.h" {
    try std.testing.expectEqual(@as(usize, 4), ofs_words.ofs_word_count);
}

test "every word defaults to the erased-MRAM value" {
    try std.testing.expectEqual(@as(u32, 0xFFFFFFFF), ofs_words.Words.ofs0);
    try std.testing.expectEqual(@as(u32, 0xFFFFFFFF), ofs_words.Words.ofs1);
    try std.testing.expectEqual(@as(u32, 0xFFFFFFFF), ofs_words.Words.ofs2);
    try std.testing.expectEqual(@as(u32, 0xFFFFFFFF), ofs_words.Words.ofs3);
    try std.testing.expectEqual(@as(u32, 0xFFFFFFFF), ofs_words.Words.sas);
    try std.testing.expectEqual(@as(u32, 0xFFFFFFFF), ofs_words.Words.ofs1_sec);
    try std.testing.expectEqual(@as(u32, 0xFFFFFFFF), ofs_words.Words.ofs1_sel);
    try std.testing.expectEqual(@as(u32, 0xFFFFFFFF), ofs_words.Words.ofs3_sec);
    try std.testing.expectEqual(@as(u32, 0xFFFFFFFF), ofs_words.Words.ofs3_sel);
    try std.testing.expectEqual(@as(u32, 0xFFFFFFFF), ofs_words.Words.bps);
    try std.testing.expectEqual(@as(u32, 0xFFFFFFFF), ofs_words.Words.bps_sec);
    try std.testing.expectEqual(@as(u32, 0xFFFFFFFF), ofs_words.Words.otp_fsblctrl0);
    try std.testing.expectEqual(@as(u32, 0xFFFFFFFF), ofs_words.Words.otp_fsblctrl1);
    try std.testing.expectEqual(@as(u32, 0xFFFFFFFF), ofs_words.Words.otp_fsblctrl2);
    try std.testing.expectEqual(@as(u32, 0xFFFFFFFF), ofs_words.Words.otp_samr);
    try std.testing.expectEqual(@as(u32, 0xFFFFFFFF), ofs_words.Words.otp_pbps);
    try std.testing.expectEqual(@as(u32, 0xFFFFFFFF), ofs_words.Words.otp_pbps_sec);
    try std.testing.expectEqual(@as(u32, 0xFFFFFFFF), ofs_words.Words.otp_zhuk);
}
