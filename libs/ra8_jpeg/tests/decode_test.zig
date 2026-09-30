//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for the baseline decoder primitives and the dimension probe
//! (#2799).

const std = @import("std");
const bitreader = @import("bitreader");
const dims = @import("dims");
const huffdec = @import("huffdec");
const idct = @import("idct");
const spec = @import("spec");
const ycc = @import("ycc");

fn readerOver(bytes: []const u8) bitreader.BitReader {
    return .{ .buf = bytes.ptr, .len = @intCast(bytes.len), .pos = 0, .acc = 0, .nbits = 0, .had_eoi = 0 };
}

test "getBits takes bits most significant first" {
    const bytes = [_]u8{ 0b1011_0010, 0b0100_0001 };
    var br = readerOver(&bytes);

    try std.testing.expectEqual(@as(u32, 0b101), br.getBits(3).?);
    try std.testing.expectEqual(@as(u32, 0b10010), br.getBits(5).?);
    try std.testing.expectEqual(@as(u32, 0b0100_0001), br.getBits(8).?);
}

test "getBits of zero width consumes nothing" {
    const bytes = [_]u8{0xAB};
    var br = readerOver(&bytes);

    try std.testing.expectEqual(@as(u32, 0), br.getBits(0).?);
    try std.testing.expectEqual(@as(u8, 0), br.nbits);
    try std.testing.expectEqual(@as(u32, 0xAB), br.getBits(8).?);
}

test "an empty stream cannot supply a bit" {
    var br = readerOver(&[_]u8{});
    try std.testing.expect(br.getBits(1) == null);
    try std.testing.expectEqual(@as(u8, 1), br.had_eoi);
}

test "a stuffed FF00 yields one FF data byte" {
    const bytes = [_]u8{ 0xFF, 0x00, 0x42 };
    var br = readerOver(&bytes);

    try std.testing.expectEqual(@as(u32, 0xFF), br.getBits(8).?);
    try std.testing.expectEqual(@as(u32, 0x42), br.getBits(8).?);
}

test "a real marker ends the stream and leaves the marker unread" {
    const bytes = [_]u8{ 0x11, 0xFF, 0xD9, 0x22 };
    var br = readerOver(&bytes);

    try std.testing.expectEqual(@as(u32, 0x11), br.getBits(8).?);
    br.fill();
    try std.testing.expectEqual(@as(u8, 1), br.had_eoi);
    // The cursor is rewound onto the 0xFF so the parser can read the marker.
    try std.testing.expectEqual(@as(u32, 1), br.pos);
}

test "a trailing FF with nothing after it ends the stream" {
    const bytes = [_]u8{ 0x11, 0xFF };
    var br = readerOver(&bytes);

    try std.testing.expectEqual(@as(u32, 0x11), br.getBits(8).?);
    br.fill();
    try std.testing.expectEqual(@as(u8, 1), br.had_eoi);
}

fn tableWith(bits: [spec.Huff.lengths]u8, vals: []const u8) huffdec.Table {
    var table: huffdec.Table = std.mem.zeroes(huffdec.Table);
    table.bits = bits;
    @memcpy(table.vals[0..vals.len], vals);
    table.build();
    return table;
}

test "build derives canonical codes for a two-length table" {
    // Two codes of length 2, one of length 3: 00, 01, 100.
    var bits = std.mem.zeroes([spec.Huff.lengths]u8);
    bits[1] = 2;
    bits[2] = 1;
    const table = tableWith(bits, &[_]u8{ 0xA0, 0xB0, 0xC0 });

    try std.testing.expectEqual(@as(u16, 3), table.total);
    try std.testing.expectEqual(@as(u16, 0b00), table.huffcode[0]);
    try std.testing.expectEqual(@as(u16, 0b01), table.huffcode[1]);
    try std.testing.expectEqual(@as(u16, 0b100), table.huffcode[2]);
    // Length 1 holds nothing, so its maxcode rejects every comparison.
    try std.testing.expectEqual(@as(i32, -1), table.maxcode[0]);
    try std.testing.expectEqual(@as(i32, 0b00), table.mincode[1]);
    try std.testing.expectEqual(@as(i32, 0b01), table.maxcode[1]);
}

test "decode reads the symbols back out of the table" {
    var bits = std.mem.zeroes([spec.Huff.lengths]u8);
    bits[1] = 2;
    bits[2] = 1;
    const table = tableWith(bits, &[_]u8{ 0xA0, 0xB0, 0xC0 });

    // 00 01 100 padded to a byte boundary: 0001_1000
    const bytes = [_]u8{0b0001_1000};
    var br = readerOver(&bytes);

    try std.testing.expectEqual(@as(u8, 0xA0), table.decode(&br).?);
    try std.testing.expectEqual(@as(u8, 0xB0), table.decode(&br).?);
    try std.testing.expectEqual(@as(u8, 0xC0), table.decode(&br).?);
}

test "decode of an empty table fails rather than reading a symbol" {
    const table = tableWith(std.mem.zeroes([spec.Huff.lengths]u8), &[_]u8{});
    const bytes = [_]u8{ 0xFF, 0x00, 0x00 };
    var br = readerOver(&bytes);

    try std.testing.expect(table.decode(&br) == null);
}

test "decode on an exhausted stream fails" {
    var bits = std.mem.zeroes([spec.Huff.lengths]u8);
    bits[1] = 2;
    const table = tableWith(bits, &[_]u8{ 0xA0, 0xB0 });

    var br = readerOver(&[_]u8{});
    try std.testing.expect(table.decode(&br) == null);
}

test "a full 256-symbol table builds without running off its arrays" {
    // The C original wrote a sentinel at huffsize[256], one past the end.
    var bits = std.mem.zeroes([spec.Huff.lengths]u8);
    bits[7] = 128;
    bits[8] = 128;
    var vals: [256]u8 = undefined;
    for (&vals, 0..) |*v, i| v.* = @intCast(i);

    const table = tableWith(bits, &vals);
    try std.testing.expectEqual(@as(u16, 256), table.total);
    try std.testing.expectEqual(@as(u8, 8), table.huffsize[127]);
    try std.testing.expectEqual(@as(u8, 9), table.huffsize[255]);
}

test "extend sign-extends only below the midpoint" {
    try std.testing.expectEqual(@as(i32, 0), huffdec.extend(0, 0));
    // 3 bits: 0..3 are negative, 4..7 stay positive.
    try std.testing.expectEqual(@as(i32, -7), huffdec.extend(0, 3));
    try std.testing.expectEqual(@as(i32, -4), huffdec.extend(3, 3));
    try std.testing.expectEqual(@as(i32, 4), huffdec.extend(4, 3));
    try std.testing.expectEqual(@as(i32, 7), huffdec.extend(7, 3));
    try std.testing.expectEqual(@as(i32, -32767), huffdec.extend(0, 15));
}

test "a DC-only block inverts to a flat plane" {
    var block = std.mem.zeroes([spec.Block.size]i32);
    block[0] = 512;
    idct.inverse(&block);

    for (block[1..]) |sample| {
        try std.testing.expectEqual(block[0], sample);
    }
}

test "an all-zero block inverts to zero" {
    var block = std.mem.zeroes([spec.Block.size]i32);
    idct.inverse(&block);
    for (block) |sample| try std.testing.expectEqual(@as(i32, 0), sample);
}

test "neutral chroma leaves grey untouched" {
    const grey = ycc.toRgb(130, spec.Block.level_offset, spec.Block.level_offset);
    try std.testing.expectEqual([_]u8{ 130, 130, 130 }, grey);
}

test "out-of-range sums clamp to the 8-bit range" {
    try std.testing.expectEqual(@as(u8, 0), ycc.clamp(-1));
    try std.testing.expectEqual(@as(u8, 255), ycc.clamp(256));
    try std.testing.expectEqual(@as(u8, 7), ycc.clamp(7));

    // Full red chroma on a mid luma saturates the red channel.
    const hot = ycc.toRgb(200, 0, 255);
    try std.testing.expectEqual(@as(u8, 255), hot[0]);
    try std.testing.expectEqual(@as(u8, 0), hot[2]);
}

/// A minimal SOI, APP0, SOF0 header carrying 7x5 pixels.
const header_7x5 = [_]u8{
    0xFF, 0xD8, // SOI
    0xFF, 0xE0, 0x00, 0x04, 0x00, 0x00, // APP0, length 4
    0xFF, 0xC0, 0x00, 0x0B, 0x08, 0x00,
    0x05, 0x00, 0x07, 0x01, 0x01, 0x11,
    0x00,
};

test "the probe finds the dimensions past an APP0" {
    const found = try dims.probe(&header_7x5);
    try std.testing.expectEqual(@as(u16, 7), found.width);
    try std.testing.expectEqual(@as(u16, 5), found.height);
}

test "a stream that does not start with SOI is rejected" {
    try std.testing.expectError(dims.Error.Protocol, dims.probe(&[_]u8{ 0xFF, 0xD9, 0x00, 0x00 }));
}

test "a progressive frame is unsupported rather than malformed" {
    var stream = header_7x5;
    stream[9] = 0xC2; // SOF0 becomes SOF2.
    try std.testing.expectError(dims.Error.Unsupported, dims.probe(&stream));
}

test "a zero dimension is a protocol error" {
    var stream = header_7x5;
    stream[16] = 0x00; // width low byte
    try std.testing.expectError(dims.Error.Protocol, dims.probe(&stream));
}

test "a sample precision other than 8 is unsupported" {
    var stream = header_7x5;
    stream[12] = 12;
    try std.testing.expectError(dims.Error.Unsupported, dims.probe(&stream));
}

test "a segment length that walks off the buffer is rejected" {
    var stream = header_7x5;
    stream[5] = 0xFF; // APP0 claims a 255-byte payload
    try std.testing.expectError(dims.Error.Protocol, dims.probe(&stream));
}

test "a run of FF fill bytes before a marker is skipped" {
    const stream = [_]u8{ 0xFF, 0xD8 } ++ [_]u8{0xFF} ** 4 ++ [_]u8{
        0xC0, 0x00, 0x0B, 0x08, 0x00, 0x05, 0x00, 0x07, 0x01, 0x01, 0x11, 0x00,
    };
    const found = try dims.probe(&stream);
    try std.testing.expectEqual(@as(u16, 7), found.width);
}

test "a stream with no SOF0 runs out and reports a protocol error" {
    const stream = [_]u8{ 0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x04, 0x00, 0x00 };
    try std.testing.expectError(dims.Error.Protocol, dims.probe(&stream));
}
