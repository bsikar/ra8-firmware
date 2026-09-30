//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Declared geometry, read at each container's own offsets. The JPEG cases
//! carry the weight: it is the only format here whose size is not at a fixed
//! offset, so the marker walk needs its own boundaries proven.

const std = @import("std");
const dims = @import("dims");

const Format = struct {
    const jpeg: u32 = 1 << 0;
    const png: u32 = 1 << 1;
    const webp: u32 = 1 << 2;
    const gif: u32 = 1 << 3;
    const bmp: u32 = 1 << 4;
};

fn pngOf(width: u32, height: u32) [24]u8 {
    var out = [_]u8{0} ** 24;
    const sig = [_]u8{ 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    @memcpy(out[0..8], &sig);
    @memcpy(out[12..16], "IHDR");
    std.mem.writeInt(u32, out[16..20], width, .big);
    std.mem.writeInt(u32, out[20..24], height, .big);
    return out;
}

test "png geometry comes from the ihdr chunk" {
    const bytes = pngOf(640, 480);
    const geom = try dims.dims(&bytes);
    try std.testing.expectEqual(Format.png, geom.format);
    try std.testing.expectEqual(@as(u32, 640), geom.width_px);
    try std.testing.expectEqual(@as(u32, 480), geom.height_px);
}

test "a png whose first chunk is not ihdr is unsupported" {
    var bytes = pngOf(64, 64);
    @memcpy(bytes[12..16], "sRGB");
    try std.testing.expectError(error.NotSupported, dims.dims(&bytes));
}

test "a png cut inside the ihdr is unsupported, not a bad signature" {
    const bytes = pngOf(64, 64);
    try std.testing.expectError(error.NotSupported, dims.dims(bytes[0..23]));
}

test "a zero png dimension is a size fault" {
    const bytes = pngOf(0, 480);
    try std.testing.expectError(error.InvalidSize, dims.dims(&bytes));
}

test "a png past dim_max is a size fault" {
    const bytes = pngOf(16385, 16);
    try std.testing.expectError(error.InvalidSize, dims.dims(&bytes));
}

test "dim_max itself is accepted" {
    const bytes = pngOf(16384, 16384);
    const geom = try dims.dims(&bytes);
    try std.testing.expectEqual(@as(u32, 16384), geom.width_px);
}

test "gif reads the logical screen descriptor, little endian" {
    var bytes = [_]u8{0} ** 10;
    @memcpy(bytes[0..6], "GIF89a");
    std.mem.writeInt(u16, bytes[6..8], 320, .little);
    std.mem.writeInt(u16, bytes[8..10], 200, .little);
    const geom = try dims.dims(&bytes);
    try std.testing.expectEqual(Format.gif, geom.format);
    try std.testing.expectEqual(@as(u32, 320), geom.width_px);
    try std.testing.expectEqual(@as(u32, 200), geom.height_px);
}

test "a gif cut inside the screen descriptor is unsupported" {
    var bytes = [_]u8{0} ** 9;
    @memcpy(bytes[0..6], "GIF89a");
    bytes[6] = 4;
    bytes[8] = 4;
    try std.testing.expectError(error.NotSupported, dims.dims(&bytes));
}

fn bmpCore(width: u16, height: u16) [22]u8 {
    var out = [_]u8{0} ** 22;
    @memcpy(out[0..2], "BM");
    std.mem.writeInt(u32, out[14..18], 12, .little);
    std.mem.writeInt(u16, out[18..20], width, .little);
    std.mem.writeInt(u16, out[20..22], height, .little);
    return out;
}

fn bmpInfo(width: i32, height: i32) [26]u8 {
    var out = [_]u8{0} ** 26;
    @memcpy(out[0..2], "BM");
    std.mem.writeInt(u32, out[14..18], 40, .little);
    std.mem.writeInt(i32, out[18..22], width, .little);
    std.mem.writeInt(i32, out[22..26], height, .little);
    return out;
}

test "the twelve-byte bmp core header carries sixteen-bit dimensions" {
    const bytes = bmpCore(24, 18);
    const geom = try dims.dims(&bytes);
    try std.testing.expectEqual(Format.bmp, geom.format);
    try std.testing.expectEqual(@as(u32, 24), geom.width_px);
    try std.testing.expectEqual(@as(u32, 18), geom.height_px);
}

test "the forty-byte bmp info header carries thirty-two-bit dimensions" {
    const bytes = bmpInfo(1024, 768);
    const geom = try dims.dims(&bytes);
    try std.testing.expectEqual(@as(u32, 1024), geom.width_px);
    try std.testing.expectEqual(@as(u32, 768), geom.height_px);
}

test "a top-down bmp stores a negative height and keeps its magnitude" {
    const bytes = bmpInfo(1024, -768);
    const geom = try dims.dims(&bytes);
    try std.testing.expectEqual(@as(u32, 768), geom.height_px);
}

test "a bmp cut before the dib size is unsupported" {
    const bytes = bmpCore(4, 4);
    try std.testing.expectError(error.NotSupported, dims.dims(bytes[0..17]));
}

test "a bmp core header cut inside its dimensions is unsupported" {
    const bytes = bmpCore(4, 4);
    try std.testing.expectError(error.NotSupported, dims.dims(bytes[0..21]));
}

fn webpOf(chunk: []const u8, payload: []const u8) [40]u8 {
    var out = [_]u8{0} ** 40;
    @memcpy(out[0..4], "RIFF");
    @memcpy(out[8..12], "WEBP");
    @memcpy(out[12..16], chunk);
    @memcpy(out[20 .. 20 + payload.len], payload);
    return out;
}

test "a lossy vp8 frame header masks each dimension to fourteen bits" {
    var payload = [_]u8{0} ** 10;
    std.mem.writeInt(u16, payload[6..8], 0xC000 | 300, .little);
    std.mem.writeInt(u16, payload[8..10], 0xC000 | 200, .little);
    const bytes = webpOf("VP8 ", &payload);
    const geom = try dims.dims(&bytes);
    try std.testing.expectEqual(Format.webp, geom.format);
    try std.testing.expectEqual(@as(u32, 300), geom.width_px);
    try std.testing.expectEqual(@as(u32, 200), geom.height_px);
}

test "a lossless vp8l stream packs both dimensions minus one into one word" {
    var payload = [_]u8{0} ** 5;
    payload[0] = 0x2F;
    const packed_dims: u32 = (299) | (@as(u32, 199) << 14);
    std.mem.writeInt(u32, payload[1..5], packed_dims, .little);
    const bytes = webpOf("VP8L", &payload);
    const geom = try dims.dims(&bytes);
    try std.testing.expectEqual(@as(u32, 300), geom.width_px);
    try std.testing.expectEqual(@as(u32, 200), geom.height_px);
}

test "a vp8l chunk without its signature byte is unsupported" {
    var payload = [_]u8{0} ** 5;
    payload[0] = 0x30;
    const bytes = webpOf("VP8L", &payload);
    try std.testing.expectError(error.NotSupported, dims.dims(&bytes));
}

test "an extended vp8x canvas stores three bytes per dimension, minus one" {
    var payload = [_]u8{0} ** 10;
    payload[4] = 0x0F;
    payload[5] = 0x27;
    payload[7] = 0x1F;
    const bytes = webpOf("VP8X", &payload);
    const geom = try dims.dims(&bytes);
    try std.testing.expectEqual(@as(u32, 10000), geom.width_px);
    try std.testing.expectEqual(@as(u32, 32), geom.height_px);
}

test "a webp whose first chunk is no vp8 flavour is unsupported" {
    const bytes = webpOf("ICCP", &[_]u8{0} ** 10);
    try std.testing.expectError(error.NotSupported, dims.dims(&bytes));
}

fn jpegSof(marker: u8, height: u16, width: u16) [13]u8 {
    var out = [_]u8{0} ** 13;
    out[0] = 0xFF;
    out[1] = 0xD8;
    out[2] = 0xFF;
    out[3] = marker;
    std.mem.writeInt(u16, out[4..6], 11, .big);
    out[6] = 8;
    std.mem.writeInt(u16, out[7..9], height, .big);
    std.mem.writeInt(u16, out[9..11], width, .big);
    return out;
}

test "the marker walk finds a baseline sof0" {
    const bytes = jpegSof(0xC0, 480, 640);
    const geom = try dims.dims(&bytes);
    try std.testing.expectEqual(Format.jpeg, geom.format);
    try std.testing.expectEqual(@as(u32, 640), geom.width_px);
    try std.testing.expectEqual(@as(u32, 480), geom.height_px);
}

test "a progressive sof2 is a frame header too" {
    const bytes = jpegSof(0xC2, 64, 128);
    const geom = try dims.dims(&bytes);
    try std.testing.expectEqual(@as(u32, 128), geom.width_px);
}

test "dht, jpg and dac sit in the sofn range without being frame headers" {
    for ([_]u8{ 0xC4, 0xC8, 0xCC }) |marker| {
        const bytes = jpegSof(marker, 64, 128);
        try std.testing.expectError(error.NotSupported, dims.dims(&bytes));
    }
}

test "a segment before the frame header is skipped by its length" {
    // SOI, APP0 of length 6, then an SOF0.
    var bytes = [_]u8{0} ** 21;
    bytes[0] = 0xFF;
    bytes[1] = 0xD8;
    bytes[2] = 0xFF;
    bytes[3] = 0xE0;
    std.mem.writeInt(u16, bytes[4..6], 6, .big);
    bytes[10] = 0xFF;
    bytes[11] = 0xC0;
    std.mem.writeInt(u16, bytes[12..14], 11, .big);
    bytes[14] = 8;
    std.mem.writeInt(u16, bytes[15..17], 100, .big);
    std.mem.writeInt(u16, bytes[17..19], 200, .big);
    const geom = try dims.dims(&bytes);
    try std.testing.expectEqual(@as(u32, 200), geom.width_px);
    try std.testing.expectEqual(@as(u32, 100), geom.height_px);
}

test "a run of 0xff before a marker is legal fill" {
    var bytes = [_]u8{0} ** 15;
    bytes[0] = 0xFF;
    bytes[1] = 0xD8;
    bytes[2] = 0xFF;
    bytes[3] = 0xFF;
    bytes[4] = 0xFF;
    bytes[5] = 0xC0;
    std.mem.writeInt(u16, bytes[6..8], 11, .big);
    bytes[8] = 8;
    std.mem.writeInt(u16, bytes[9..11], 24, .big);
    std.mem.writeInt(u16, bytes[11..13], 32, .big);
    const geom = try dims.dims(&bytes);
    try std.testing.expectEqual(@as(u32, 32), geom.width_px);
    try std.testing.expectEqual(@as(u32, 24), geom.height_px);
}

test "reaching the scan with no frame header is unsupported" {
    const bytes = [_]u8{ 0xFF, 0xD8, 0xFF, 0xDA, 0x00, 0x0C };
    try std.testing.expectError(error.NotSupported, dims.dims(&bytes));
}

test "reaching the end of image with no frame header is unsupported" {
    const bytes = [_]u8{ 0xFF, 0xD8, 0xFF, 0xD9 };
    try std.testing.expectError(error.NotSupported, dims.dims(&bytes));
}

test "a restart marker is standalone and carries no length to skip" {
    var bytes = [_]u8{0} ** 15;
    bytes[0] = 0xFF;
    bytes[1] = 0xD8;
    bytes[2] = 0xFF;
    bytes[3] = 0xD0;
    bytes[4] = 0xFF;
    bytes[5] = 0xC0;
    std.mem.writeInt(u16, bytes[6..8], 11, .big);
    bytes[8] = 8;
    std.mem.writeInt(u16, bytes[9..11], 10, .big);
    std.mem.writeInt(u16, bytes[11..13], 20, .big);
    const geom = try dims.dims(&bytes);
    try std.testing.expectEqual(@as(u32, 20), geom.width_px);
}

test "a desynchronised byte where a marker should be is unsupported" {
    // SOI, then an APP0 of length 4, then a byte that is not a marker prefix.
    const bytes = [_]u8{ 0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x04, 0x00, 0x00, 0x00, 0xC0 };
    try std.testing.expectError(error.NotSupported, dims.dims(&bytes));
}

test "a segment length below two is malformed" {
    const bytes = [_]u8{ 0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x01 };
    try std.testing.expectError(error.NotSupported, dims.dims(&bytes));
}

test "an sof segment too short to hold a size is unsupported" {
    var bytes = jpegSof(0xC0, 64, 64);
    std.mem.writeInt(u16, bytes[4..6], 6, .big);
    try std.testing.expectError(error.NotSupported, dims.dims(&bytes));
}

test "an sof truncated by the buffer is unsupported" {
    const bytes = jpegSof(0xC0, 64, 64);
    try std.testing.expectError(error.NotSupported, dims.dims(bytes[0..10]));
}

test "running out of bytes before any frame header is unsupported" {
    const bytes = [_]u8{ 0xFF, 0xD8, 0xFF };
    try std.testing.expectError(error.NotSupported, dims.dims(&bytes));
}

test "unrecognised bytes are not found, which is not the same as unsupported" {
    try std.testing.expectError(error.NotFound, dims.dims(&[_]u8{ 1, 2, 3, 4 }));
}

test "an empty buffer is a size fault" {
    try std.testing.expectError(error.InvalidSize, dims.dims(&[_]u8{}));
}

test "tga is sniffable by nothing, so it never reaches a reader" {
    const tga = [_]u8{0} ** 18;
    try std.testing.expectError(error.NotFound, dims.dims(&tga));
}
