//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The default-deny rule in front of the copy-to-run hand-off.

const std = @import("std");
const gate = @import("launch_gate");

test "only an exact success passes" {
    try std.testing.expect(gate.passed(gate.ok));
}

test "every known failure denies" {
    const crc_mismatch: u16 = 0x405;
    const validation_failed: u16 = 0x501;
    const checksum_mismatch: u16 = 0x502;
    const null_ptr: u16 = 0x504;
    for ([_]u16{ crc_mismatch, validation_failed, checksum_mismatch, null_ptr }) |verdict| {
        try std.testing.expect(!gate.passed(verdict));
    }
}

test "an unrecognised verdict denies" {
    try std.testing.expect(!gate.passed(1));
    try std.testing.expect(!gate.passed(0xFFFF));
}
