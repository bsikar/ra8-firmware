//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The host-capabilities announcement, checked against the protocol rather
//! than against the code that writes it.

const std = @import("std");
const testing = std.testing;

const implementation = @import("implementation");
const caps = implementation.caps;
const Caps = caps.Caps;
const Tag = caps.Tag;

test "the frame is two header octets and five three-octet tags" {
    try testing.expectEqual(@as(u8, 17), Caps.bytes);
    try testing.expectEqual(@as(u8, Caps.hdr + Caps.tags * Caps.stride), Caps.bytes);
}

test "a full-size buffer takes the whole announcement" {
    var out: [Caps.bytes]u8 = @splat(0);
    try testing.expectEqual(@as(?u8, Caps.bytes), caps.write(&out));
}

test "a buffer one octet short is refused and left untouched" {
    var out: [Caps.bytes - 1]u8 = @splat(0xCC);
    try testing.expectEqual(@as(?u8, null), caps.write(&out));
    for (out) |byte| try testing.expectEqual(@as(u8, 0xCC), byte);
}

test "the frame announces itself as an init event of the tag bytes that follow" {
    var out: [Caps.bytes]u8 = @splat(0);
    _ = caps.write(&out).?;
    try testing.expectEqual(Tag.event_init, out[Caps.type_at]);
    try testing.expectEqual(@as(u8, 15), out[Caps.len_at]);
}

test "the five tags are emitted in upstream's order with their values" {
    var out: [Caps.bytes]u8 = @splat(0);
    _ = caps.write(&out).?;

    const expected = [_][2]u8{
        .{ Tag.host_capabilities, Caps.host },
        .{ Tag.chip_id, Caps.chip },
        .{ Tag.test_raw_tp, Caps.raw_tp },
        .{ Tag.throttle_high, Caps.throttle_high },
        .{ Tag.throttle_low, Caps.throttle_low },
    };
    for (expected, 0..) |want, i| {
        const at = Caps.hdr + i * Caps.stride;
        try testing.expectEqual(want[0], out[at]);
        try testing.expectEqual(Caps.value_len, out[at + 1]);
        try testing.expectEqual(want[1], out[at + 2]);
    }
}

test "the tag identifiers are the vendored enum, consecutive from host capabilities" {
    try testing.expectEqual(@as(u8, 0x44), Tag.host_capabilities);
    try testing.expectEqual(@as(u8, 0x45), Tag.chip_id);
    try testing.expectEqual(@as(u8, 0x46), Tag.test_raw_tp);
    try testing.expectEqual(@as(u8, 0x47), Tag.throttle_high);
    try testing.expectEqual(@as(u8, 0x48), Tag.throttle_low);
    try testing.expectEqual(@as(u8, 0x22), Tag.event_init);
}

test "the part announced is the ESP32-C6 this board carries" {
    try testing.expectEqual(@as(u8, 0x0D), Caps.chip);
}

test "the flow-control marks leave the co-processor room to recover" {
    try testing.expect(Caps.throttle_high > Caps.throttle_low);
    try testing.expectEqual(@as(u8, 80), Caps.throttle_high);
    try testing.expectEqual(@as(u8, 60), Caps.throttle_low);
}

test "a longer buffer is written exactly as far as the frame reaches" {
    var out: [Caps.bytes + 8]u8 = @splat(0xEE);
    try testing.expectEqual(@as(?u8, Caps.bytes), caps.write(&out));
    for (out[Caps.bytes..]) |byte| try testing.expectEqual(@as(u8, 0xEE), byte);
}
