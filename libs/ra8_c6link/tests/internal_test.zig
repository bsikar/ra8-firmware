//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Tests for the envelope codec itself, in slices: what `open` lays down,
//! what `body` accepts, and every malformed shape it has to reject.

const std = @import("std");
const implementation = @import("implementation");

const tlv = implementation.tlv;
const vocab = implementation.vocab;
const Ep = vocab.Ep;
const Tag = vocab.Tag;
const Envelope = vocab.Envelope;

/// Build a well-formed envelope around `payload_bytes` body octets.
fn sealed(buf: []u8, body_len: u16, fill: u8) []const u8 {
    const at = tlv.open(buf, body_len).?;
    @memset(buf[at..][0..body_len], fill);
    return buf[0 .. at + body_len];
}

test "the envelope costs twelve bytes" {
    try std.testing.expectEqual(@as(u16, 6), Ep.len);
    try std.testing.expectEqual(@as(u16, 9), Envelope.data_tag);
    try std.testing.expectEqual(@as(u16, 12), Envelope.overhead);
}

test "open writes both tag headers and the endpoint name" {
    var buf: [300]u8 = undefined;
    const at = tlv.open(&buf, 0x0102).?;

    try std.testing.expectEqual(Envelope.overhead, at);
    try std.testing.expectEqual(Tag.epname, buf[0]);
    try std.testing.expectEqual(@as(u8, 6), buf[1]);
    try std.testing.expectEqual(@as(u8, 0), buf[2]);
    try std.testing.expectEqualSlices(u8, Ep.rsp, buf[3..9]);
    try std.testing.expectEqual(Tag.data, buf[9]);
    try std.testing.expectEqual(@as(u8, 0x02), buf[10]);
    try std.testing.expectEqual(@as(u8, 0x01), buf[11]);
}

test "open refuses a buffer one byte short and accepts an exact fit" {
    var buf: [32]u8 = undefined;
    try std.testing.expectEqual(@as(?u16, null), tlv.open(buf[0 .. Envelope.overhead + 4 - 1], 4));
    try std.testing.expect(tlv.open(buf[0 .. Envelope.overhead + 4], 4) != null);
}

test "open refuses a body that would overflow a 16-bit span" {
    var buf: [32]u8 = undefined;
    try std.testing.expectEqual(@as(?u16, null), tlv.open(&buf, 0xFFFF));
}

test "body round-trips what open laid down" {
    var buf: [64]u8 = undefined;
    const frame = sealed(&buf, 5, 0xAB);

    const found = tlv.body(frame).?;
    try std.testing.expectEqual(@as(usize, 5), found.len);
    try std.testing.expectEqualSlices(u8, &.{ 0xAB, 0xAB, 0xAB, 0xAB, 0xAB }, found);
    try std.testing.expectEqual(&frame[Envelope.overhead], &found[0]);
}

test "body accepts an event envelope as well as a response" {
    var buf: [64]u8 = undefined;
    _ = sealed(&buf, 2, 0x11);
    @memcpy(buf[Tag.value..][0..Ep.len], Ep.evt);

    const found = tlv.body(buf[0 .. Envelope.overhead + 2]).?;
    try std.testing.expectEqual(@as(usize, 2), found.len);
}

test "body rejects a payload shorter than the envelope" {
    var buf: [64]u8 = undefined;
    _ = sealed(&buf, 4, 0x22);
    try std.testing.expectEqual(@as(?[]const u8, null), tlv.body(buf[0 .. Envelope.overhead - 1]));
}

test "body rejects a wrong tag type on either tag" {
    var buf: [64]u8 = undefined;
    const len = sealed(&buf, 4, 0x22).len;

    buf[Tag.type_at] = Tag.data;
    try std.testing.expectEqual(@as(?[]const u8, null), tlv.body(buf[0..len]));

    buf[Tag.type_at] = Tag.epname;
    buf[Envelope.data_tag] = Tag.epname;
    try std.testing.expectEqual(@as(?[]const u8, null), tlv.body(buf[0..len]));
}

test "body rejects an endpoint-name length that is not six" {
    var buf: [64]u8 = undefined;
    const len = sealed(&buf, 4, 0x22).len;
    buf[Tag.len_lo] = 5;
    try std.testing.expectEqual(@as(?[]const u8, null), tlv.body(buf[0..len]));
}

test "body rejects an unknown endpoint name, one octet is enough" {
    var buf: [64]u8 = undefined;
    const len = sealed(&buf, 4, 0x22).len;
    buf[Tag.value + 3] = 'X';
    try std.testing.expectEqual(@as(?[]const u8, null), tlv.body(buf[0..len]));
}

test "a name mixed from both endpoints is accepted, one octet at a time" {
    var buf: [64]u8 = undefined;
    const len = sealed(&buf, 4, 0x22).len;
    buf[Tag.value + 3] = Ep.evt[3];
    try std.testing.expect(tlv.body(buf[0..len]) != null);
}

test "body rejects an empty body" {
    var buf: [64]u8 = undefined;
    _ = tlv.open(&buf, 0).?;
    try std.testing.expectEqual(@as(?[]const u8, null), tlv.body(buf[0..Envelope.overhead]));
}

test "body rejects a declared body longer than the payload" {
    var buf: [64]u8 = undefined;
    const len = sealed(&buf, 4, 0x22).len;
    buf[Envelope.data_tag + Tag.len_lo] = 5;
    try std.testing.expectEqual(@as(?[]const u8, null), tlv.body(buf[0..len]));
}

test "body reads the declared length little-endian" {
    var buf: [600]u8 = undefined;
    const frame = sealed(&buf, 0x0101, 0x33);
    const found = tlv.body(frame).?;
    try std.testing.expectEqual(@as(usize, 0x0101), found.len);
}

test "body ignores octets past the declared body" {
    var buf: [64]u8 = undefined;
    _ = sealed(&buf, 3, 0x44);
    const found = tlv.body(buf[0 .. Envelope.overhead + 8]).?;
    try std.testing.expectEqual(@as(usize, 3), found.len);
}
