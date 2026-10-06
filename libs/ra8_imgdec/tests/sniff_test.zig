//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The container sniff: one signature test, where the tree used to carry four
//! that disagreed.

const std = @import("std");
const sniff = @import("sniff");

const Format = struct {
    const jpeg: u32 = 1 << 0;
    const png: u32 = 1 << 1;
    const webp: u32 = 1 << 2;
    const gif: u32 = 1 << 3;
    const bmp: u32 = 1 << 4;
};

const png_head = [_]u8{ 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };

test "an empty buffer is a size fault, not a missing signature" {
    try std.testing.expectError(error.InvalidSize, sniff.sniff(&[_]u8{}));
}

test "png is recognised by its eight-byte signature" {
    try std.testing.expectEqual(Format.png, try sniff.sniff(&png_head));
}

test "a png signature one byte short is not found" {
    try std.testing.expectError(error.NotFound, sniff.sniff(png_head[0..7]));
}

test "one wrong byte in the png signature is not found" {
    var broken = png_head;
    broken[3] = 'X';
    try std.testing.expectError(error.NotFound, sniff.sniff(&broken));
}

test "jpeg is soi plus the first marker prefix" {
    try std.testing.expectEqual(Format.jpeg, try sniff.sniff(&[_]u8{ 0xFF, 0xD8, 0xFF, 0xE0 }));
}

test "soi alone is too short to answer" {
    try std.testing.expectError(error.NotFound, sniff.sniff(&[_]u8{ 0xFF, 0xD8 }));
}

test "webp needs both the riff tag and the webp form type" {
    try std.testing.expectEqual(Format.webp, try sniff.sniff("RIFF\x00\x00\x00\x00WEBP"));
}

test "riff without the webp form type is not a webp" {
    try std.testing.expectError(error.NotFound, sniff.sniff("RIFF\x00\x00\x00\x00WAVE"));
}

test "a riff header cut before the form type is not found" {
    try std.testing.expectError(error.NotFound, sniff.sniff("RIFF\x00\x00\x00\x00WEB"));
}

test "both gif versions are recognised" {
    try std.testing.expectEqual(Format.gif, try sniff.sniff("GIF87a"));
    try std.testing.expectEqual(Format.gif, try sniff.sniff("GIF89a"));
}

test "a gif with an unknown version trailer is not found" {
    try std.testing.expectError(error.NotFound, sniff.sniff("GIF88a"));
}

test "bmp is two bytes" {
    try std.testing.expectEqual(Format.bmp, try sniff.sniff("BM"));
}

test "tga has no signature, so it is never sniffed" {
    // A plausible 18-byte TGA header: no signature anywhere in it.
    const tga: [18]u8 = @splat(0);
    try std.testing.expectError(error.NotFound, sniff.sniff(&tga));
}

test "png is tested before jpeg, so neither shadows the other" {
    try std.testing.expectEqual(Format.png, try sniff.sniff(&png_head));
    try std.testing.expectEqual(Format.jpeg, try sniff.sniff(&[_]u8{ 0xFF, 0xD8, 0xFF }));
}
