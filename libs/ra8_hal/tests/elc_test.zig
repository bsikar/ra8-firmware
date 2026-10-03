//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const elc = @import("elc");

var regs: [0x110]u8 align(4) = undefined;

fn fake() elc.Window {
    @memset(&regs, 0xAA);
    return .{ .base = @intFromPtr(&regs) };
}

fn elsr(index: usize) u16 {
    return std.mem.readInt(u16, regs[0x20 + 4 * index ..][0..2], .little);
}

test "clearRoutes zeroes ELSR0..52 and unlocks ELSEGR0..3 only" {
    _ = fake();
    elc.clearRoutes(.{ .base = @intFromPtr(&regs) });
    for (0..elc.elsr_count) |i| try std.testing.expectEqual(@as(u16, 0), elsr(i));
    for (0..elc.segr_count) |g| try std.testing.expectEqual(elc.step_unlock, regs[4 + 4 * g]);
    try std.testing.expectEqual(@as(u8, 0xAA), regs[0]);
    try std.testing.expectEqual(@as(u8, 0xAA), regs[0x20 + 4 * 53]);
    try std.testing.expectEqual(@as(u8, 0xAA), regs[0x22]);
}

test "setEnabled writes ELCON and isEnabled reads only bit 7" {
    const w = fake();
    elc.setEnabled(w, true);
    try std.testing.expectEqual(@as(u8, 0x80), regs[0]);
    try std.testing.expect(elc.isEnabled(w));
    regs[0] = 0x7F;
    try std.testing.expect(!elc.isEnabled(w));
    elc.setEnabled(w, false);
    try std.testing.expectEqual(@as(u8, 0), regs[0]);
}

test "link writes the event to ELSRn and refuses index 53" {
    const w = fake();
    try elc.link(w, 0, 0x338);
    try elc.link(w, 52, 0x067);
    try std.testing.expectEqual(@as(u16, 0x338), elsr(0));
    try std.testing.expectEqual(@as(u16, 0x067), elsr(52));
    try std.testing.expectError(error.OutOfRange, elc.link(w, 53, 1));
}

test "unlink clears ELSRn and refuses index 53" {
    const w = fake();
    try elc.unlink(w, 7);
    try std.testing.expectEqual(@as(u16, 0), elsr(7));
    try std.testing.expectEqual(@as(u16, 0xAAAA), elsr(6));
    try std.testing.expectError(error.OutOfRange, elc.unlink(w, 53));
}

test "trigger ends on the SEG write and refuses group 4" {
    const w = fake();
    try elc.trigger(w, 3);
    try std.testing.expectEqual(elc.step_trigger, regs[0x10]);
    try std.testing.expectEqual(@as(u8, 0xAA), regs[0x0C]);
    try std.testing.expectError(error.InvalidArg, elc.trigger(w, 4));
}
