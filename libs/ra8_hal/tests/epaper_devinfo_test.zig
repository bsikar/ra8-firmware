//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const devinfo = @import("epaper_devinfo");

fn response() [devinfo.word_count]u16 {
    var w: [devinfo.word_count]u16 = @splat(0);
    w[0] = 1872;
    w[1] = 1404;
    w[2] = 0x2345;
    w[3] = 0x0011;
    const fw = "SWv_0.1.1.0.....";
    const lut = "M641\x01\x7F..........";
    for (0..8) |i| w[4 + i] = (@as(u16, fw[2 * i]) << 8) | fw[2 * i + 1];
    for (0..8) |i| w[12 + i] = (@as(u16, lut[2 * i]) << 8) | lut[2 * i + 1];
    return w;
}

test "decode takes width, height and the buffer base low word first" {
    const info = devinfo.decode(&response());
    try std.testing.expectEqual(@as(u16, 1872), info.panel_width);
    try std.testing.expectEqual(@as(u16, 1404), info.panel_height);
    try std.testing.expectEqual(@as(u32, 0x0011_2345), info.image_buf_base);
}

test "decode unpacks the FW version high byte first and terminates it" {
    const info = devinfo.decode(&response());
    try std.testing.expectEqualStrings("SWv_0.1.1.0.....", info.fw_version[0..16]);
    try std.testing.expectEqual(@as(u8, 0), info.fw_version[16]);
}

test "decode turns non-printable LUT bytes into NUL" {
    const info = devinfo.decode(&response());
    try std.testing.expectEqualStrings("M641", info.lut_version[0..4]);
    try std.testing.expectEqual(@as(u8, 0), info.lut_version[4]);
    try std.testing.expectEqual(@as(u8, 0), info.lut_version[5]);
    try std.testing.expectEqual(@as(u8, '.'), info.lut_version[6]);
    try std.testing.expectEqual(@as(u8, 0), info.lut_version[16]);
}

test "printable keeps 0x20..0x7E only" {
    try std.testing.expectEqual(@as(u8, 0x20), devinfo.printable(0x20));
    try std.testing.expectEqual(@as(u8, 0x7E), devinfo.printable(0x7E));
    try std.testing.expectEqual(@as(u8, 0), devinfo.printable(0x1F));
    try std.testing.expectEqual(@as(u8, 0), devinfo.printable(0x7F));
    try std.testing.expectEqual(@as(u8, 0), devinfo.printable(0xC8));
}

test "DevInfo has the C struct's size and layout" {
    try std.testing.expectEqual(@as(usize, 44), @sizeOf(devinfo.DevInfo));
    try std.testing.expectEqual(@as(usize, 4), @offsetOf(devinfo.DevInfo, "image_buf_base"));
}
