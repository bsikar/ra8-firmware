//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The board pin tables: every id resolves, out-of-range ids are rejected, and
//! the provisional pin/channel values are pinned so a silent edit fails here.

const std = @import("std");
const pins = @import("pins");

const vocab = pins.vocab;

const Pin = vocab.Pin;

test "led ids resolve to the provisional EK-RA8D2 pins" {
    try std.testing.expectEqual(Pin.pack(6, 0), pins.ledPin(pins.Led.led1).?);
    try std.testing.expectEqual(Pin.pack(3, 3), pins.ledPin(pins.Led.led2).?);
    try std.testing.expectEqual(Pin.pack(10, 7), pins.ledPin(pins.Led.led3).?);
}

test "led ids past the count are rejected" {
    try std.testing.expectEqual(@as(?u16, null), pins.ledPin(pins.Led.count));
    try std.testing.expectEqual(@as(?u16, null), pins.ledPin(0xFF));
}

test "switch ids resolve to pins, irq channels and elc events" {
    try std.testing.expectEqual(Pin.pack(0, 9), pins.swPin(pins.Sw.sw1).?);
    try std.testing.expectEqual(Pin.pack(0, 8), pins.swPin(pins.Sw.sw2).?);
    try std.testing.expectEqual(@as(u8, 13), pins.swIrqNum(pins.Sw.sw1).?);
    try std.testing.expectEqual(@as(u8, 12), pins.swIrqNum(pins.Sw.sw2).?);
    try std.testing.expectEqual(pins.SwEvent.irq13, pins.swEvent(pins.Sw.sw1).?);
    try std.testing.expectEqual(pins.SwEvent.irq12, pins.swEvent(pins.Sw.sw2).?);
}

test "switch ids past the count are rejected on every lookup" {
    try std.testing.expectEqual(@as(?u16, null), pins.swPin(pins.Sw.count));
    try std.testing.expectEqual(@as(?u8, null), pins.swIrqNum(pins.Sw.count));
    try std.testing.expectEqual(@as(?u16, null), pins.swEvent(pins.Sw.count));
}

test "pin packing round-trips port and index" {
    const packed_pin = Pin.pack(13, 2);
    try std.testing.expectEqual(@as(u8, 13), Pin.port(packed_pin));
    try std.testing.expectEqual(@as(u8, 2), Pin.index(packed_pin));
}

test "buttons are active low" {
    try std.testing.expect(pins.pressed(vocab.Level.low));
    try std.testing.expect(!pins.pressed(vocab.Level.high));
}
