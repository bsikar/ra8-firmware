//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The J1 pin tables and the rule that picks the GLCDC outputs out of them.

const std = @import("std");
const glcdc_pins = @import("glcdc_pins");

fn find(table: []const glcdc_pins.Entry, signal: []const u8) ?u16 {
    for (table) |entry| {
        if (std.mem.eql(u8, entry.signal, signal)) return entry.pin;
    }
    return null;
}

test "table sizes match the UM connector table" {
    try std.testing.expectEqual(@as(usize, 35), glcdc_pins.rgb888.len);
    try std.testing.expectEqual(@as(usize, 29), glcdc_pins.rgb666.len);
    try std.testing.expectEqual(@as(usize, 27), glcdc_pins.rgb565.len);
}

test "tableFor maps the format ids and refuses anything else" {
    try std.testing.expectEqual(@as(usize, 35), (glcdc_pins.tableFor(0) orelse unreachable).len);
    try std.testing.expectEqual(@as(usize, 29), (glcdc_pins.tableFor(1) orelse unreachable).len);
    try std.testing.expectEqual(@as(usize, 27), (glcdc_pins.tableFor(2) orelse unreachable).len);
    try std.testing.expect(glcdc_pins.tableFor(3) == null);
    try std.testing.expect(glcdc_pins.tableFor(255) == null);
}

test "every table carries the same eleven non-GLCDC control lines" {
    const control = [_][]const u8{ "BLEN", "SDA1", "INT", "SCL1", "RST", "EXTCLK" };
    for ([_][]const glcdc_pins.Entry{ &glcdc_pins.rgb888, &glcdc_pins.rgb666, &glcdc_pins.rgb565 }) |table| {
        for (control) |signal| try std.testing.expect(find(table, signal) != null);
        try std.testing.expectEqual(@as(?u16, 0x050F), find(table, "CLK"));
        try std.testing.expectEqual(@as(?u16, 0x0606), find(table, "RST"));
        try std.testing.expectEqual(@as(?u16, 0x050E), find(table, "BLEN"));
    }
}

fn dataPins(table: []const glcdc_pins.Entry, out: []u16) usize {
    var n: usize = 0;
    for (table) |entry| {
        if (!glcdc_pins.isColorData(entry.signal)) continue;
        out[n] = entry.pin;
        n += 1;
    }
    return n;
}

test "the data lines are one ordered bus, and a narrower format uses its first N pins" {
    // The panel's colour bits are not per-channel pins: GLCDC drives one
    // contiguous data bus, so 18-bit mode takes the first 18 lines of the
    // 24-bit order and renames them, rather than dropping two pins per
    // channel. RGB666's R7 therefore sits on RGB888's R1 pin.
    var wide: [32]u16 = undefined;
    var mid: [32]u16 = undefined;
    var narrow: [32]u16 = undefined;
    const n_wide = dataPins(&glcdc_pins.rgb888, &wide);
    const n_mid = dataPins(&glcdc_pins.rgb666, &mid);
    const n_narrow = dataPins(&glcdc_pins.rgb565, &narrow);

    try std.testing.expectEqual(@as(usize, 24), n_wide);
    try std.testing.expectEqual(@as(usize, 18), n_mid);
    try std.testing.expectEqual(@as(usize, 16), n_narrow);
    try std.testing.expectEqualSlices(u16, wide[0..n_mid], mid[0..n_mid]);
    try std.testing.expectEqualSlices(u16, wide[0..n_narrow], narrow[0..n_narrow]);
}

test "the renaming is what shifts, so the same pin changes channel" {
    // P11_0 is R1 at 24-bit and R7 at 18-bit. Catching this is the point of
    // keeping three explicit tables instead of deriving two from one.
    try std.testing.expectEqual(@as(?u16, 0x0B00), find(&glcdc_pins.rgb888, "R1"));
    try std.testing.expectEqual(@as(?u16, 0x0B00), find(&glcdc_pins.rgb666, "R7"));
    try std.testing.expectEqual(find(&glcdc_pins.rgb888, "B0"), find(&glcdc_pins.rgb666, "B2"));
}

test "RGB565 carries six green bits and five each of red and blue" {
    try std.testing.expect(find(&glcdc_pins.rgb565, "G2") != null);
    try std.testing.expect(find(&glcdc_pins.rgb565, "G7") != null);
    try std.testing.expect(find(&glcdc_pins.rgb565, "R3") != null);
    try std.testing.expect(find(&glcdc_pins.rgb565, "R2") == null);
    try std.testing.expect(find(&glcdc_pins.rgb565, "B2") == null);
}

test "isColorData needs a digit, so BLEN is not blue bit something" {
    try std.testing.expect(glcdc_pins.isColorData("B0"));
    try std.testing.expect(glcdc_pins.isColorData("G7"));
    try std.testing.expect(glcdc_pins.isColorData("R15"));
    try std.testing.expect(!glcdc_pins.isColorData("BLEN"));
    try std.testing.expect(!glcdc_pins.isColorData("RST"));
    try std.testing.expect(!glcdc_pins.isColorData("B"));
    try std.testing.expect(!glcdc_pins.isColorData(""));
    try std.testing.expect(!glcdc_pins.isColorData("SDA1"));
}

test "isOutput takes TCON, CLK and colour data and nothing else" {
    try std.testing.expect(glcdc_pins.isOutput("TCON0"));
    try std.testing.expect(glcdc_pins.isOutput("TCON3"));
    try std.testing.expect(glcdc_pins.isOutput("CLK"));
    try std.testing.expect(glcdc_pins.isOutput("R7"));
    try std.testing.expect(!glcdc_pins.isOutput("EXTCLK"));
    try std.testing.expect(!glcdc_pins.isOutput("BLEN"));
    try std.testing.expect(!glcdc_pins.isOutput("RST"));
    try std.testing.expect(!glcdc_pins.isOutput("SDA1"));
    try std.testing.expect(!glcdc_pins.isOutput("SCL1"));
    try std.testing.expect(!glcdc_pins.isOutput("INT"));
}

test "EXTCLK is matched on its prefix, not on ending in CLK" {
    // The rule is startsWith("CLK"): the panel's clock input must stay off
    // the route list even though its name ends in CLK.
    try std.testing.expect(!glcdc_pins.isOutput("EXTCLK"));
    try std.testing.expect(glcdc_pins.isOutput("CLK"));
}

test "the routed set is 24 pins for RGB888 and shrinks with the format" {
    var counts: [3]usize = .{ 0, 0, 0 };
    for ([_][]const glcdc_pins.Entry{ &glcdc_pins.rgb888, &glcdc_pins.rgb666, &glcdc_pins.rgb565 }, 0..) |table, i| {
        for (table) |entry| {
            if (glcdc_pins.isOutput(entry.signal)) counts[i] += 1;
        }
    }
    // 24 data + CLK + 4 TCON, then 18 + 5, then 16 + 5.
    try std.testing.expectEqual(@as(usize, 29), counts[0]);
    try std.testing.expectEqual(@as(usize, 23), counts[1]);
    try std.testing.expectEqual(@as(usize, 21), counts[2]);
}

test "no table repeats a pin" {
    for ([_][]const glcdc_pins.Entry{ &glcdc_pins.rgb888, &glcdc_pins.rgb666, &glcdc_pins.rgb565 }) |table| {
        for (table, 0..) |a, i| {
            for (table[i + 1 ..]) |b| try std.testing.expect(a.pin != b.pin);
        }
    }
}
