//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const ids = @import("exit_stop");

test "ids match the ra8_mstp_regs.h packing" {
    try std.testing.expectEqual(@as(u16, 0x0104), ids.i3c);
    try std.testing.expectEqual(@as(u16, 0x0204), ids.glcdc);
    try std.testing.expectEqual(@as(u16, 0x0210), ids.ceu);
    try std.testing.expectEqual(@as(u16, 0x021E), ids.eswm);
    try std.testing.expectEqual(@as(u16, 0x0315), ids.adc16h);
}

test "ids are distinct apart from the shared ESWM" {
    const all = [_]u16{ ids.i3c, ids.glcdc, ids.ceu, ids.eswm, ids.adc16h };
    for (all, 0..) |a, i| {
        for (all[i + 1 ..]) |b| try std.testing.expect(a != b);
    }
}
