//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const ofs_words = @import("ofs_words");

test "the OFS quartet inventory matches ra8_ofs.h" {
    try std.testing.expectEqual(@as(usize, 4), ofs_words.ofs_word_count);
}

test "every word defaults to the erased-MRAM value" {
    try std.testing.expectEqual(@as(u32, 0xFFFFFFFF), ofs_words.Words.ofs0);
    try std.testing.expectEqual(@as(u32, 0xFFFFFFFF), ofs_words.Words.ofs1_sec);
    try std.testing.expectEqual(@as(u32, 0xFFFFFFFF), ofs_words.Words.otp_zhuk);
}
