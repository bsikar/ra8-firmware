//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host unit tests for `ra8_nsc_appimg_verify` TT-range decision and wire ABI.

const std = @import("std");
const verify = @import("verify");

test "range_accepts rejects zero length and null pointers" {
    try std.testing.expect(!verify.range_accepts(0x3000_0000, 0, verify.tt_read_bit, verify.tt_read_bit));
    try std.testing.expect(!verify.range_accepts(0, 16, verify.tt_read_bit, verify.tt_read_bit));
}

test "range_accepts rejects 32-bit address wraparound" {
    try std.testing.expect(!verify.range_accepts(0xFFFF_FFF0, 32, verify.tt_read_bit, verify.tt_read_bit));
    try std.testing.expect(verify.range_accepts(0xFFFF_FFF0, 16, verify.tt_read_bit, verify.tt_read_bit));
}

test "range_accepts rejects unreadable TT probe info" {
    try std.testing.expect(!verify.range_accepts(0x3000_0000, 32, 0, 0));
    try std.testing.expect(!verify.range_accepts(0x3000_0000, 32, ~verify.tt_read_bit, ~verify.tt_read_bit));
}

test "range_accepts enforces single-region TT match across 32-byte granules" {
    // Within one 32-byte aligned granule, last_info is not required to match.
    try std.testing.expect(verify.range_accepts(0x3000_0000, 32, verify.tt_read_bit, 0));
    // Crossing a 32-byte granule requires identical TT info on start and end.
    try std.testing.expect(!verify.range_accepts(0x3000_0000, 33, verify.tt_read_bit, verify.tt_read_bit | 1));
    try std.testing.expect(verify.range_accepts(0x3000_0000, 64, verify.tt_read_bit | 7, verify.tt_read_bit | 7));
}

test "WireMsg layout and shared veneer error codes match C ABI expectations" {
    try std.testing.expectEqual(@as(usize, 2 * @sizeOf(usize) + 8), @sizeOf(verify.WireMsg));
    try std.testing.expectEqual(@as(usize, 0), @offsetOf(verify.WireMsg, "head"));
    try std.testing.expectEqual(@as(usize, @sizeOf(usize)), @offsetOf(verify.WireMsg, "tail"));
    try std.testing.expectEqual(@as(usize, 2 * @sizeOf(usize)), @offsetOf(verify.WireMsg, "head_len"));
    try std.testing.expectEqual(@as(usize, 2 * @sizeOf(usize) + 4), @offsetOf(verify.WireMsg, "tail_len"));
    try std.testing.expectEqual(@as(u16, 0x103), verify.k_err_invalid_arg);
    try std.testing.expectEqual(@as(u16, 0x504), verify.k_err_null_ptr);
}
