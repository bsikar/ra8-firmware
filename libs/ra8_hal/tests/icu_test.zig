//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const icu = @import("icu");

var regs: [icu.off_wupen1 + 4]u8 align(4) = undefined;

fn fake() icu.Window {
    @memset(&regs, 0xAA);
    return .{ .base = @intFromPtr(&regs) };
}

fn word(off: usize) u32 {
    return std.mem.readInt(u32, regs[off..][0..4], .little);
}

test "irqcr maps channels 0..15 to IRQCRa and 16..31 to IRQCRb, past NMICR" {
    const w = fake();
    try std.testing.expectEqual(w.base + 0x00, @intFromPtr(try w.irqcr(0)));
    try std.testing.expectEqual(w.base + 0x0F, @intFromPtr(try w.irqcr(15)));
    try std.testing.expectEqual(w.base + 0x14, @intFromPtr(try w.irqcr(16)));
    try std.testing.expectEqual(w.base + 0x23, @intFromPtr(try w.irqcr(31)));
    try std.testing.expectError(error.InvalidIrq, w.irqcr(32));
}

test "init clears every IRQCR, NMIER and WUPEN, and writes NMICLR all-ones" {
    const w = fake();
    icu.init(w);
    for (regs[0x00..0x10]) |b| try std.testing.expectEqual(@as(u8, 0), b);
    try std.testing.expectEqual(@as(u8, 0xAA), regs[0x10]); // NMICR untouched
    for (regs[0x14..0x24]) |b| try std.testing.expectEqual(@as(u8, 0), b);
    try std.testing.expectEqual(@as(u32, 0), word(icu.off_nmier));
    try std.testing.expectEqual(icu.nmiclr_all, word(icu.off_nmiclr));
    try std.testing.expectEqual(@as(u32, 0), word(icu.off_wupen0));
    try std.testing.expectEqual(@as(u32, 0), word(icu.off_wupen1));
}

test "irqcrValue packs IRQMD, FCLKSEL and FLTEN and masks overwide fields" {
    try std.testing.expectEqual(@as(u8, 0x00), icu.irqcrValue(.{ .sense = 0, .filter_div = 0, .filter_en = false }));
    try std.testing.expectEqual(@as(u8, 0xB3), icu.irqcrValue(.{ .sense = 3, .filter_div = 3, .filter_en = true }));
    try std.testing.expectEqual(@as(u8, 0x12), icu.irqcrValue(.{ .sense = 0xFE, .filter_div = 0x11, .filter_en = false }));
}

test "configureIrqPin writes the encoded value and readIrqcr reads it back" {
    const w = fake();
    try icu.configureIrqPin(w, 20, .{ .sense = 1, .filter_div = 2, .filter_en = true });
    try std.testing.expectEqual(@as(u8, 0xA1), regs[0x14 + 4]);
    try std.testing.expectEqual(@as(u8, 0xA1), try icu.readIrqcr(w, 20));
    try std.testing.expectError(error.InvalidIrq, icu.configureIrqPin(w, 32, .{ .sense = 0, .filter_div = 0, .filter_en = false }));
    try std.testing.expectError(error.InvalidIrq, icu.readIrqcr(w, 255));
}

test "NMI enable and disable read-modify-write NMIER; clear and status hit their own registers" {
    const w = fake();
    icu.init(w);
    icu.nmiEnable(w, 0x0005);
    icu.nmiEnable(w, 0x0100);
    try std.testing.expectEqual(@as(u32, 0x0105), word(icu.off_nmier));
    icu.nmiDisable(w, 0x0001);
    try std.testing.expectEqual(@as(u32, 0x0104), word(icu.off_nmier));
    icu.nmiClear(w, 0x0010);
    try std.testing.expectEqual(@as(u32, 0x0010), word(icu.off_nmiclr));
    std.mem.writeInt(u32, regs[icu.off_nmisr..][0..4], 0x8001, .little);
    try std.testing.expectEqual(@as(u32, 0x8001), icu.nmiStatus(w));
}

test "Cfg matches the 3-byte ra8_icu_irq_cfg_t" {
    try std.testing.expectEqual(@as(usize, 3), @sizeOf(icu.Cfg));
    try std.testing.expectEqual(@as(usize, 2), @offsetOf(icu.Cfg, "filter_en"));
}
