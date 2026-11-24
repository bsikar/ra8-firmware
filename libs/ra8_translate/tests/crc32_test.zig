//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Unit tests for the IEEE CRC-32 helper used by `TRN1` sidecars.

const std = @import("std");
const crc32 = @import("crc32");

test "crc32 matches standard IEEE 802.3 test vector" {
    try std.testing.expectEqual(@as(u32, 0x00000000), crc32.crc32(""));
    try std.testing.expectEqual(@as(u32, 0xCBF43926), crc32.crc32("123456789"));
}

test "crc32WithZeroWindow ignores stored checksum window" {
    var a = [_]u8{ 1, 2, 3, 4, 0, 0, 0, 0, 5, 6, 7, 8 };
    var b = [_]u8{ 1, 2, 3, 4, 0xDE, 0xAD, 0xBE, 0xEF, 5, 6, 7, 8 };

    const expected = crc32.crc32(&a);
    try std.testing.expectEqual(expected, crc32.crc32WithZeroWindow(&a, 4, 4));
    try std.testing.expectEqual(expected, crc32.crc32WithZeroWindow(&b, 4, 4));
}
