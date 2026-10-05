//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const gate = @import("isr_globals");

var enables: u32 = 0;
var disables: u32 = 0;

const FakeHw = struct {
    pub fn irqEnable() void {
        enables += 1;
    }
    pub fn irqDisable() void {
        disables += 1;
    }
};

test "enable clears PRIMASK through the seam once" {
    enables = 0;
    disables = 0;
    gate.enable(FakeHw);
    try std.testing.expectEqual(@as(u32, 1), enables);
    try std.testing.expectEqual(@as(u32, 0), disables);
}

test "disable sets PRIMASK through the seam once" {
    enables = 0;
    disables = 0;
    gate.disable(FakeHw);
    try std.testing.expectEqual(@as(u32, 0), enables);
    try std.testing.expectEqual(@as(u32, 1), disables);
}

test "a critical section pairs disable then enable" {
    enables = 0;
    disables = 0;
    gate.disable(FakeHw);
    gate.enable(FakeHw);
    try std.testing.expectEqual(@as(u32, 1), enables);
    try std.testing.expectEqual(@as(u32, 1), disables);
}
