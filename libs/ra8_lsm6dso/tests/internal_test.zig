//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Zig-native tests for the pure core: the CTRL1_XL / CTRL2_G encoders, the
//! little-endian sample decoders, the temperature conversion and the FIFO
//! depth arithmetic. No transport is involved anywhere in this file.

const std = @import("std");
const testing = std.testing;
const implementation = @import("implementation");

test "gyro FS: 125 dps takes the FS_125 bit and no FS_G field" {
    try testing.expectEqual(@as(u8, 0x02), implementation.gyroFsBits(0x00));
}

test "gyro FS: 250 dps encodes FS_G = 0 with FS_125 clear" {
    try testing.expectEqual(@as(u8, 0x00), implementation.gyroFsBits(0x01));
}

test "gyro FS: 500 dps encodes FS_G = 1" {
    try testing.expectEqual(@as(u8, 0x04), implementation.gyroFsBits(0x02));
}

test "gyro FS: 1000 dps encodes FS_G = 2" {
    try testing.expectEqual(@as(u8, 0x08), implementation.gyroFsBits(0x03));
}

test "gyro FS: 2000 dps encodes FS_G = 3" {
    try testing.expectEqual(@as(u8, 0x0C), implementation.gyroFsBits(0x04));
}

test "gyro FS: every accepted code lands inside bits [3:1]" {
    var fs: u8 = 0;
    while (fs <= implementation.g_fs_cap) : (fs += 1) {
        const bits = implementation.gyroFsBits(fs);
        try testing.expectEqual(@as(u8, 0), bits & ~implementation.mask_fs_g_full);
    }
}

test "accel FS merge keeps the ODR nibble and the untouched low bits" {
    // ODR = 4 (104 Hz) pre-seeded, FS_XL currently 0b11.
    const merged = implementation.accelFsMerge(0x4D, 0x02);
    try testing.expectEqual(@as(u8, 0x49), merged);
}

test "accel FS merge masks the code down to two bits" {
    try testing.expectEqual(@as(u8, 0x0C), implementation.accelFsMerge(0x00, 0x07));
}

test "accel FS merge writes 2 g as an all-clear field" {
    try testing.expectEqual(@as(u8, 0xF0), implementation.accelFsMerge(0xFC, 0x00));
}

test "gyro FS merge clears FS_G and FS_125 before OR-ing the new field" {
    // CTRL2_G holding ODR 5, FS_G = 3, FS_125 = 1 -> retarget at 250 dps.
    try testing.expectEqual(@as(u8, 0x50), implementation.gyroFsMerge(0x5E, 0x01));
}

test "gyro FS merge preserves the ODR nibble when selecting 125 dps" {
    try testing.expectEqual(@as(u8, 0xA2), implementation.gyroFsMerge(0xA0, 0x00));
}

test "ODR bits shift the code into [7:4]" {
    try testing.expectEqual(@as(u8, 0xA0), implementation.odrBits(0x0A));
    try testing.expectEqual(@as(u8, 0x10), implementation.odrBits(0x01));
    try testing.expectEqual(@as(u8, 0x00), implementation.odrBits(0x00));
}

test "ODR bits mask the code to a nibble" {
    try testing.expectEqual(@as(u8, 0x30), implementation.odrBits(0xF3));
}

test "ODR merge replaces [7:4] and keeps [3:0]" {
    try testing.expectEqual(@as(u8, 0x6C), implementation.odrMerge(0x2C, 0x60));
}

test "ODR merge to power-down clears the nibble only" {
    try testing.expectEqual(@as(u8, 0x0E), implementation.odrMerge(0xFE, 0x00));
}

test "ODR merge over a full sweep never disturbs the low nibble" {
    var odr: u8 = 0;
    while (odr <= implementation.odr_cap) : (odr += 1) {
        const merged = implementation.odrMerge(0x0D, implementation.odrBits(odr));
        try testing.expectEqual(@as(u8, 0x0D), merged & implementation.mask_nibble);
        try testing.expectEqual(odr, merged >> 4);
    }
}

test "little-endian combine: positive sample" {
    try testing.expectEqual(@as(i16, 0x1234), implementation.combineLe(0x34, 0x12));
}

test "little-endian combine: minus one" {
    try testing.expectEqual(@as(i16, -1), implementation.combineLe(0xFF, 0xFF));
}

test "little-endian combine: most negative sample" {
    try testing.expectEqual(@as(i16, -32768), implementation.combineLe(0x00, 0x80));
}

test "little-endian combine: most positive sample" {
    try testing.expectEqual(@as(i16, 32767), implementation.combineLe(0xFF, 0x7F));
}

test "little-endian combine: zero" {
    try testing.expectEqual(@as(i16, 0), implementation.combineLe(0x00, 0x00));
}

test "XYZ unpack reads the three axes in burst order" {
    const bytes = [6]u8{ 0x34, 0x12, 0xFF, 0xFF, 0x00, 0x80 };
    const sample = implementation.unpackXyz(&bytes);
    try testing.expectEqual(@as(i16, 0x1234), sample.x);
    try testing.expectEqual(@as(i16, -1), sample.y);
    try testing.expectEqual(@as(i16, -32768), sample.z);
}

test "XYZ unpack of an all-zero burst is the origin" {
    const bytes: [6]u8 = @splat(0);
    const sample = implementation.unpackXyz(&bytes);
    try testing.expectEqual(@as(i16, 0), sample.x);
    try testing.expectEqual(@as(i16, 0), sample.y);
    try testing.expectEqual(@as(i16, 0), sample.z);
}

test "temperature: raw zero is the +25 C zero offset" {
    try testing.expectEqual(@as(i32, 2500), implementation.tempCentiC(0));
}

test "temperature: one full LSB step is +1 C" {
    try testing.expectEqual(@as(i32, 2600), implementation.tempCentiC(256));
}

test "temperature: negative raw sits below the offset" {
    try testing.expectEqual(@as(i32, 2400), implementation.tempCentiC(-256));
}

test "temperature: division truncates toward zero on both signs" {
    try testing.expectEqual(@as(i32, 2500), implementation.tempCentiC(1));
    try testing.expectEqual(@as(i32, 2500), implementation.tempCentiC(-1));
}

test "temperature: extremes stay in range" {
    try testing.expectEqual(@as(i32, 2500 + 12799), implementation.tempCentiC(32767));
    try testing.expectEqual(@as(i32, 2500 - 12800), implementation.tempCentiC(-32768));
}

test "FIFO depth combines the low byte with the status2 nibble" {
    try testing.expectEqual(@as(u32, 0x123), implementation.fifoDepth(0x23, 0x01));
}

test "FIFO depth ignores the upper nibble of FIFO_STATUS2" {
    try testing.expectEqual(@as(u32, 0x2AA), implementation.fifoDepth(0xAA, 0xF2));
}

test "FIFO depth saturates the 10-bit field at 1023" {
    try testing.expectEqual(@as(u32, 1023), implementation.fifoDepth(0xFF, 0x03));
}

test "FIFO depth of an empty FIFO is zero" {
    try testing.expectEqual(@as(u32, 0), implementation.fifoDepth(0x00, 0xF0));
}

test "words to read clamps the live depth to the caller cap" {
    try testing.expectEqual(@as(u32, 4), implementation.wordsToRead(9, 4));
}

test "words to read passes a shallow FIFO through untouched" {
    try testing.expectEqual(@as(u32, 3), implementation.wordsToRead(3, 128));
}

test "words to read: equal depth and cap reads everything" {
    try testing.expectEqual(@as(u32, 7), implementation.wordsToRead(7, 7));
}

test "words to read: an empty FIFO reads nothing" {
    try testing.expectEqual(@as(u32, 0), implementation.wordsToRead(0, 128));
}

test "FIFO byte span is seven bytes per record" {
    try testing.expectEqual(@as(u32, 7), implementation.fifoTotalBytes(1));
    try testing.expectEqual(@as(u32, 896), implementation.fifoTotalBytes(128));
    try testing.expectEqual(@as(u32, 7161), implementation.fifoTotalBytes(1023));
}

test "register map matches DS12140" {
    try testing.expectEqual(@as(u8, 0x0F), implementation.reg_who_am_i);
    try testing.expectEqual(@as(u8, 0x10), implementation.reg_ctrl1_xl);
    try testing.expectEqual(@as(u8, 0x11), implementation.reg_ctrl2_g);
    try testing.expectEqual(@as(u8, 0x20), implementation.reg_out_temp_l);
    try testing.expectEqual(@as(u8, 0x22), implementation.reg_outx_l_g);
    try testing.expectEqual(@as(u8, 0x28), implementation.reg_outx_l_a);
    try testing.expectEqual(@as(u8, 0x3A), implementation.reg_fifo_status1);
    try testing.expectEqual(@as(u8, 0x78), implementation.reg_fifo_data_out);
    try testing.expectEqual(@as(u8, 0x6C), implementation.who_am_i_value);
}
