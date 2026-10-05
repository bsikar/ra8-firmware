//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const irq = @import("i3c_i2c_irq");

test "attach enables the BIE and NTIE groups" {
    var bie: u32 = 0;
    var ntie: u32 = 0;
    irq.setEnables(&bie, &ntie, true);
    try std.testing.expectEqual(@as(u32, 0x0011_0110), bie);
    try std.testing.expectEqual(@as(u32, 0x3), ntie);
}

test "detach clears both groups" {
    var bie: u32 = 0xFFFF_FFFF;
    var ntie: u32 = 0xFFFF_FFFF;
    irq.setEnables(&bie, &ntie, false);
    try std.testing.expectEqual(@as(u32, 0), bie);
    try std.testing.expectEqual(@as(u32, 0), ntie);
}

test "dispatch needs both a mask and a handler" {
    try std.testing.expect(irq.shouldDispatch(0x02, true));
    try std.testing.expect(!irq.shouldDispatch(0x00, true));
    try std.testing.expect(!irq.shouldDispatch(0x02, false));
    try std.testing.expect(!irq.shouldDispatch(0x00, false));
}
