//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The payload header: what gets written, and what gets believed.

const std = @import("std");
const testing = std.testing;

const implementation = @import("implementation");
const frame = implementation.frame;
const Frame = frame.Frame;
const Hdr = frame.Hdr;

/// A transaction buffer, the only size the header writer accepts.
fn buffer() [Frame.bytes]u8 {
    return @as([Frame.bytes]u8, @splat(0));
}

fn readLe(buf: []const u8, at: u16) u16 {
    return std.mem.readInt(u16, buf[at..][0..2], .little);
}

test "the geometry is the frame less the header" {
    try testing.expectEqual(@as(u16, 1600), Frame.bytes);
    try testing.expectEqual(@as(u16, 12), Frame.header_bytes);
    try testing.expectEqual(@as(u16, 1588), Frame.max_payload);
}

test "the filler frame is addressed to no interface and is otherwise zero" {
    var tx = buffer();
    @memset(&tx, 0xAA);
    frame.filler(&tx);

    try testing.expectEqual(frame.Iface.max, tx[Hdr.iface]);
    for (tx[1..]) |byte| try testing.expectEqual(@as(u8, 0), byte);
}

test "a sealed frame carries the length, the offset and a zero sequence" {
    var tx = buffer();
    try testing.expect(frame.seal(&tx, 5, 0, 40));

    try testing.expectEqual(@as(u16, 40), readLe(&tx, Hdr.len));
    try testing.expectEqual(Frame.header_bytes, readLe(&tx, Hdr.offset));
    try testing.expectEqual(@as(u16, 0), readLe(&tx, Hdr.seq_num));
    try testing.expectEqual(@as(u8, 0), tx[Hdr.flags]);
}

test "both interface fields share the first octet, low nibble first" {
    var tx = buffer();
    try testing.expect(frame.seal(&tx, 0x0C, 0x03, 0));
    try testing.expectEqual(@as(u8, 0x3C), tx[Hdr.iface]);
}

test "an interface value wider than a nibble is masked, not truncated into its neighbour" {
    var tx = buffer();
    try testing.expect(frame.seal(&tx, 0xF5, 0xF2, 0));
    try testing.expectEqual(@as(u8, 0x25), tx[Hdr.iface]);
}

test "sealing preserves a payload already staged behind the header" {
    var tx = buffer();
    for (0..8) |i| tx[Frame.header_bytes + i] = @intCast(0x10 + i);
    tx[Frame.header_bytes + 8] = 0xFF;

    try testing.expect(frame.seal(&tx, 1, 0, 8));

    for (0..8) |i| try testing.expectEqual(@as(u8, @intCast(0x10 + i)), tx[Frame.header_bytes + i]);
    try testing.expectEqual(@as(u8, 0), tx[Frame.header_bytes + 8]);
}

test "a payload past the maximum is refused and the buffer left alone" {
    var tx = buffer();
    @memset(&tx, 0x7E);
    try testing.expect(!frame.seal(&tx, 1, 0, Frame.max_payload + 1));
    for (tx) |byte| try testing.expectEqual(@as(u8, 0x7E), byte);
}

test "the largest payload the geometry allows still seals" {
    var tx = buffer();
    try testing.expect(frame.seal(&tx, 1, 0, Frame.max_payload));
    try testing.expectEqual(Frame.max_payload, readLe(&tx, Hdr.len));
}

test "a frame the writer sealed classifies as data" {
    var tx = buffer();
    for (0..16) |i| tx[Frame.header_bytes + i] = @intCast(i);
    try testing.expect(frame.seal(&tx, 3, 1, 16));

    const got = frame.classify(&tx);
    try testing.expectEqual(Frame.header_bytes, got.data.offset);
    try testing.expectEqual(@as(u16, 16), got.data.len);
    try testing.expectEqual(@as(u8, 3), got.data.if_type);
    try testing.expectEqual(@as(u8, 1), got.data.if_num);
}

test "the classifier does not write to the frame it is judging" {
    var tx = buffer();
    try testing.expect(frame.seal(&tx, 3, 0, 32));
    const before = tx;

    _ = frame.classify(&tx);
    try testing.expectEqualSlices(u8, &before, &tx);
}

test "a zero length is the idle filler, not a defect, even with a zero offset" {
    var tx = buffer();
    frame.filler(&tx);
    try testing.expectEqual(frame.Class.idle, frame.classify(&tx));
}

test "an offset that is not the header size is malformed" {
    var tx = buffer();
    try testing.expect(frame.seal(&tx, 1, 0, 8));
    std.mem.writeInt(u16, tx[Hdr.offset..][0..2], Frame.header_bytes + 1, .little);
    try testing.expectEqual(frame.Class.malformed, frame.classify(&tx));
}

test "a length past the maximum payload is malformed" {
    var tx = buffer();
    try testing.expect(frame.seal(&tx, 1, 0, 8));
    std.mem.writeInt(u16, tx[Hdr.len..][0..2], Frame.max_payload + 1, .little);
    try testing.expectEqual(frame.Class.malformed, frame.classify(&tx));
}

test "the offset is checked before the length, so a bad offset wins" {
    var tx = buffer();
    try testing.expect(frame.seal(&tx, 1, 0, 8));
    std.mem.writeInt(u16, tx[Hdr.offset..][0..2], 0, .little);
    std.mem.writeInt(u16, tx[Hdr.len..][0..2], Frame.max_payload + 1, .little);
    try testing.expectEqual(frame.Class.malformed, frame.classify(&tx));
}

test "a flipped payload octet is caught by the checksum" {
    var tx = buffer();
    try testing.expect(frame.seal(&tx, 1, 0, 8));
    tx[Frame.header_bytes] ^= 0x01;
    try testing.expectEqual(frame.Class.bad_checksum, frame.classify(&tx));
}

test "a wrong transmitted checksum is caught" {
    var tx = buffer();
    try testing.expect(frame.seal(&tx, 1, 0, 8));
    const claimed = readLe(&tx, Hdr.checksum);
    std.mem.writeInt(u16, tx[Hdr.checksum..][0..2], claimed +% 1, .little);
    try testing.expectEqual(frame.Class.bad_checksum, frame.classify(&tx));
}

test "the checksum is the wrapping sum of the span with the field taken as zero" {
    var tx = buffer();
    for (0..4) |i| tx[Frame.header_bytes + i] = 0xFF;
    try testing.expect(frame.seal(&tx, 2, 0, 4));

    var expected: u16 = 0;
    const span = Frame.header_bytes + 4;
    for (tx[0..span], 0..) |byte, i| {
        if (i == Hdr.checksum or i == Hdr.checksum + 1) continue;
        expected +%= byte;
    }
    try testing.expectEqual(expected, readLe(&tx, Hdr.checksum));
}

test "the checksum covers only the declared span, not the whole transaction" {
    var tx = buffer();
    try testing.expect(frame.seal(&tx, 1, 0, 4));
    const claimed = readLe(&tx, Hdr.checksum);

    tx[Frame.header_bytes + 4] = 0x5A;
    try testing.expectEqual(claimed, readLe(&tx, Hdr.checksum));
    try testing.expectEqual(@as(u16, 4), frame.classify(&tx).data.len);
}

test "a checksum that wraps past 16 bits still verifies" {
    var tx = buffer();
    for (0..600) |i| tx[Frame.header_bytes + i] = 0xFF;
    try testing.expect(frame.seal(&tx, 1, 0, 600));
    try testing.expectEqual(@as(u16, 600), frame.classify(&tx).data.len);
}

test "the interface nibbles survive a round trip through the wire layout" {
    var tx = buffer();
    for (0..16) |type_nibble| {
        for (0..16) |num_nibble| {
            try testing.expect(frame.seal(&tx, @intCast(type_nibble), @intCast(num_nibble), 1));
            const got = frame.classify(&tx).data;
            try testing.expectEqual(@as(u8, @intCast(type_nibble)), got.if_type);
            try testing.expectEqual(@as(u8, @intCast(num_nibble)), got.if_num);
        }
    }
}
