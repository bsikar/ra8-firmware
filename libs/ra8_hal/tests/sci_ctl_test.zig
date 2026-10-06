//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/sci_ctl.zig (RA8FW-906).

const std = @import("std");
const ctl = @import("sci_ctl");

test "errMask folds ORER FER PER" {
    try std.testing.expectEqual(@as(u8, 0), ctl.errMask(0));
    try std.testing.expectEqual(@as(u8, 0x01), ctl.errMask(1 << 24));
    try std.testing.expectEqual(@as(u8, 0x02), ctl.errMask(1 << 28));
    try std.testing.expectEqual(@as(u8, 0x04), ctl.errMask(1 << 27));
    try std.testing.expectEqual(@as(u8, 0x07), ctl.errMask(0xFFFF_FFFF));
}

test "clear mask is ORERC PERC FERC" {
    try std.testing.expectEqual(@as(u32, 0x1900_0000), ctl.clear_mask);
}

test "withIe sets and clears only its bit" {
    try std.testing.expectEqual(@as(u32, 0x0001_0030), ctl.withIe(0x30, ctl.ccr0_rie, true));
    try std.testing.expectEqual(@as(u32, 0x0000_0030), ctl.withIe(0x0011_0030, ctl.ccr0_rie | ctl.ccr0_tie, false));
    try std.testing.expectEqual(@as(u32, 0x0010_0000), ctl.withIe(0, ctl.ccr0_tie, true));
}

test "mstpId matches MSTPB31..22" {
    try std.testing.expectEqual(@as(u16, 0x11F), ctl.mstpId(0));
    try std.testing.expectEqual(@as(u16, 0x11E), ctl.mstpId(1));
    try std.testing.expectEqual(@as(u16, 0x116), ctl.mstpId(9));
}
