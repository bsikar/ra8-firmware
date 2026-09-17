//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Behavioural regression tests for the blob `gen_jpeg_fixture` writes (#858).
//!
//! The seeds live in the committed libFuzzer corpora, so the contract this
//! migration had to preserve is byte-for-byte output, not "a JPEG the parser
//! accepts". The 8x8 golden below is the exact blob the deleted Python
//! implementation emitted; the rest of the cases pin the segments around it so
//! a future edit that changes a table has to change a test that names it.

const std = @import("std");
const jpeg = @import("implementation");

/// The exact 346 bytes `gen_jpeg_fixture.py --width 8 --height 8` emitted,
/// captured from the Python implementation before it was deleted.
const golden_8x8_hex =
    "ffd8ffe000104a46494600010100000100010000ffdb004300010101010101010101010101" ++
    "01010101010101010101010101010101010101010101010101010101010101010101010101" ++
    "010101010101010101010101010101ffc0000b080008000801011100ffc4001f0000010501" ++
    "010101010100000000000000000102030405060708090a0bffc400b5100002010303020403" ++
    "050504040000017d000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c" ++
    "1d1e1f202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f4041" ++
    "42434445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f60616263646566" ++
    "6768696a6b6c6d6e6f707172737475767778797a7b7c7d7e7f808182838485868788898a8b" ++
    "8c8d8e8f909192939495969798999a9b9c9d9e9fa0a1ffda0008010100003f000000000000" ++
    "0000000000000000000000ffd9";

fn golden(allocator: std.mem.Allocator) ![]u8 {
    const bytes = try allocator.alloc(u8, golden_8x8_hex.len / 2);
    errdefer allocator.free(bytes);
    return std.fmt.hexToBytes(bytes, golden_8x8_hex);
}

fn build(width: u32, height: u32) ![]u8 {
    return jpeg.buildMinimalJpeg(std.testing.allocator, width, height);
}

test "an 8x8 seed is byte-identical to the implementation this replaced" {
    const blob = try build(8, 8);
    defer std.testing.allocator.free(blob);
    const expected = try golden(std.testing.allocator);
    defer std.testing.allocator.free(expected);
    try std.testing.expectEqualSlices(u8, expected, blob);
}

test "every seed is the documented length" {
    const blob = try build(8, 8);
    defer std.testing.allocator.free(blob);
    try std.testing.expectEqual(jpeg.blob_len, blob.len);
}

test "the length does not vary with the dimensions" {
    const small = try build(1, 1);
    defer std.testing.allocator.free(small);
    const large = try build(65535, 65535);
    defer std.testing.allocator.free(large);
    try std.testing.expectEqual(small.len, large.len);
}

test "the stream opens with SOI" {
    const blob = try build(8, 8);
    defer std.testing.allocator.free(blob);
    try std.testing.expectEqualSlices(u8, &[_]u8{ 0xFF, 0xD8 }, blob[0..2]);
}

test "the stream closes with EOI" {
    const blob = try build(8, 8);
    defer std.testing.allocator.free(blob);
    try std.testing.expectEqualSlices(u8, &[_]u8{ 0xFF, 0xD9 }, blob[blob.len - 2 ..]);
}

test "SOF0 carries height then width, big-endian" {
    const blob = try build(32, 24);
    defer std.testing.allocator.free(blob);
    const sof0 = std.mem.indexOf(u8, blob, &[_]u8{ 0xFF, 0xC0 }).?;
    // marker(2) length(2) precision(1), then height, then width.
    try std.testing.expectEqualSlices(u8, &[_]u8{ 0x00, 0x18 }, blob[sof0 + 5 ..][0..2]);
    try std.testing.expectEqualSlices(u8, &[_]u8{ 0x00, 0x20 }, blob[sof0 + 7 ..][0..2]);
}

test "a dimension above a byte is not truncated" {
    const blob = try build(0x1234, 0x4321);
    defer std.testing.allocator.free(blob);
    const sof0 = std.mem.indexOf(u8, blob, &[_]u8{ 0xFF, 0xC0 }).?;
    try std.testing.expectEqualSlices(u8, &[_]u8{ 0x43, 0x21 }, blob[sof0 + 5 ..][0..2]);
    try std.testing.expectEqualSlices(u8, &[_]u8{ 0x12, 0x34 }, blob[sof0 + 7 ..][0..2]);
}

test "only the SOF0 dimension bytes vary between two sizes" {
    const first = try build(8, 8);
    defer std.testing.allocator.free(first);
    const second = try build(16, 16);
    defer std.testing.allocator.free(second);

    var differing = std.ArrayList(usize).init(std.testing.allocator);
    defer differing.deinit();
    for (first, second, 0..) |left, right, index| {
        if (left != right) try differing.append(index);
    }

    const sof0 = std.mem.indexOf(u8, first, &[_]u8{ 0xFF, 0xC0 }).?;
    try std.testing.expectEqual(@as(usize, 2), differing.items.len);
    try std.testing.expectEqual(sof0 + 6, differing.items[0]);
    try std.testing.expectEqual(sof0 + 8, differing.items[1]);
}

test "the smallest expressible dimensions are accepted" {
    const blob = try build(1, 1);
    defer std.testing.allocator.free(blob);
    try std.testing.expectEqual(jpeg.blob_len, blob.len);
}

test "the largest expressible dimensions are accepted" {
    const blob = try build(65535, 65535);
    defer std.testing.allocator.free(blob);
    const sof0 = std.mem.indexOf(u8, blob, &[_]u8{ 0xFF, 0xC0 }).?;
    try std.testing.expectEqualSlices(u8, &[_]u8{ 0xFF, 0xFF }, blob[sof0 + 5 ..][0..2]);
}

test "a zero width is refused" {
    try std.testing.expectError(jpeg.Error.DimensionOutOfRange, build(0, 8));
}

test "a zero height is refused" {
    try std.testing.expectError(jpeg.Error.DimensionOutOfRange, build(8, 0));
}

test "a width above the SOF0 field is refused" {
    try std.testing.expectError(jpeg.Error.DimensionOutOfRange, build(65536, 8));
}

test "a height above the SOF0 field is refused" {
    try std.testing.expectError(jpeg.Error.DimensionOutOfRange, build(8, 65536));
}

test "APP0 declares JFIF 1.01 with 1:1 density and no thumbnail" {
    const blob = try build(8, 8);
    defer std.testing.allocator.free(blob);
    const app0 = std.mem.indexOf(u8, blob, &[_]u8{ 0xFF, 0xE0 }).?;
    try std.testing.expectEqualSlices(u8, &[_]u8{ 0x00, 0x10 }, blob[app0 + 2 ..][0..2]);
    try std.testing.expectEqualSlices(u8, "JFIF\x00", blob[app0 + 4 ..][0..5]);
    try std.testing.expectEqualSlices(
        u8,
        &[_]u8{ 0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00 },
        blob[app0 + 9 ..][0..9],
    );
}

test "the quantisation table is one 8-bit luma table of ones" {
    const blob = try build(8, 8);
    defer std.testing.allocator.free(blob);
    const dqt = std.mem.indexOf(u8, blob, &[_]u8{ 0xFF, 0xDB }).?;
    try std.testing.expectEqualSlices(u8, &[_]u8{ 0x00, 0x43 }, blob[dqt + 2 ..][0..2]);
    try std.testing.expectEqual(@as(u8, 0x00), blob[dqt + 4]);
    for (blob[dqt + 5 ..][0..64]) |coefficient| {
        try std.testing.expectEqual(@as(u8, 0x01), coefficient);
    }
}

test "the DC Huffman table carries its sixteen counts and twelve symbols" {
    const blob = try build(8, 8);
    defer std.testing.allocator.free(blob);
    const dht = std.mem.indexOf(u8, blob, &[_]u8{ 0xFF, 0xC4 }).?;
    try std.testing.expectEqualSlices(u8, &[_]u8{ 0x00, 0x1F }, blob[dht + 2 ..][0..2]);
    try std.testing.expectEqual(@as(u8, 0x00), blob[dht + 4]);
    try std.testing.expectEqualSlices(
        u8,
        &[_]u8{ 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0 },
        blob[dht + 5 ..][0..16],
    );
    for (blob[dht + 21 ..][0..12], 0..) |symbol, index| {
        try std.testing.expectEqual(@as(u8, @intCast(index)), symbol);
    }
}

test "the AC Huffman table carries its sixteen counts and 162 symbols" {
    const blob = try build(8, 8);
    defer std.testing.allocator.free(blob);
    const first = std.mem.indexOf(u8, blob, &[_]u8{ 0xFF, 0xC4 }).?;
    const dht = std.mem.indexOfPos(u8, blob, first + 2, &[_]u8{ 0xFF, 0xC4 }).?;
    try std.testing.expectEqualSlices(u8, &[_]u8{ 0x00, 0xB5 }, blob[dht + 2 ..][0..2]);
    try std.testing.expectEqual(@as(u8, 0x10), blob[dht + 4]);
    try std.testing.expectEqualSlices(
        u8,
        &[_]u8{ 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7D },
        blob[dht + 5 ..][0..16],
    );
    for (blob[dht + 21 ..][0..0xA2], 0..) |symbol, index| {
        try std.testing.expectEqual(@as(u8, @intCast(index)), symbol);
    }
}

test "the scan header names one component over the full spectrum" {
    const blob = try build(8, 8);
    defer std.testing.allocator.free(blob);
    const sos = std.mem.indexOf(u8, blob, &[_]u8{ 0xFF, 0xDA }).?;
    try std.testing.expectEqualSlices(
        u8,
        &[_]u8{ 0x00, 0x08, 0x01, 0x01, 0x00, 0x00, 0x3F, 0x00 },
        blob[sos + 2 ..][0..8],
    );
}

test "the entropy segment is sixteen bytes needing no 0xFF stuffing" {
    const blob = try build(8, 8);
    defer std.testing.allocator.free(blob);
    const sos = std.mem.indexOf(u8, blob, &[_]u8{ 0xFF, 0xDA }).?;
    const entropy = blob[sos + 10 .. blob.len - 2];
    try std.testing.expectEqual(@as(usize, 16), entropy.len);
    for (entropy) |byte| try std.testing.expectEqual(@as(u8, 0x00), byte);
}

test "every segment length field describes the payload that follows it" {
    const blob = try build(8, 8);
    defer std.testing.allocator.free(blob);

    var index: usize = 2; // past SOI
    var segments: usize = 0;
    while (index + 4 <= blob.len) {
        const marker = blob[index + 1];
        if (blob[index] != 0xFF or marker == 0xDA or marker == 0xD9) break;
        const length = std.mem.readInt(u16, blob[index + 2 ..][0..2], .big);
        try std.testing.expect(length >= 2);
        index += 2 + length;
        segments += 1;
    }
    // APP0, DQT, SOF0, DHT, DHT.
    try std.testing.expectEqual(@as(usize, 5), segments);
    try std.testing.expectEqualSlices(u8, &[_]u8{ 0xFF, 0xDA }, blob[index..][0..2]);
}
