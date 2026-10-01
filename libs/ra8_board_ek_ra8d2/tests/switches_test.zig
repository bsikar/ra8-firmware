//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Button pins, the active-low read, and the IRQ attach path.

const std = @import("std");
const switches = @import("switches");

var input_pin: u16 = 0xFFFF;
var input_pull: u32 = 0xFFFF_FFFF;
var read_level: u32 = 1;
var read_err: u32 = 0;
var icu_irq: u8 = 0xFF;
var icu_sense: u8 = 0xFF;
var icu_filter_div: u8 = 0xFF;
var icu_filter_en: bool = false;
var icu_calls: u32 = 0;
var icu_err: u32 = 0;
var isr_event: u16 = 0xFFFF;
var isr_prio: u8 = 0xFF;
var isr_ctx: ?*anyopaque = null;
var isr_slot_ptr_was_null: bool = false;
var isr_calls: u32 = 0;

const Cfg = extern struct { sense: u8, filter_div: u8, filter_en: bool };

export fn ra8_gpio_input_init(pin: u16, pull: u32) u32 {
    input_pin = pin;
    input_pull = pull;
    return 0;
}

export fn ra8_gpio_read(pin: u16, out_level: *u32) u32 {
    input_pin = pin;
    out_level.* = read_level;
    return read_err;
}

export fn ra8_icu_configure_irq_pin(irq_num: u8, cfg: *const Cfg) u32 {
    icu_calls += 1;
    icu_irq = irq_num;
    icu_sense = cfg.sense;
    icu_filter_div = cfg.filter_div;
    icu_filter_en = cfg.filter_en;
    return icu_err;
}

export fn ra8_isr_register(
    event: u16,
    handler: *const fn (?*anyopaque) callconv(.c) void,
    ctx: ?*anyopaque,
    priority: u8,
    out_slot: ?*u16,
) u32 {
    _ = handler;
    isr_calls += 1;
    isr_event = event;
    isr_ctx = ctx;
    isr_prio = priority;
    isr_slot_ptr_was_null = out_slot == null;
    return 0;
}

fn noopHandler(_: ?*anyopaque) callconv(.c) void {}

fn reset() void {
    input_pin = 0xFFFF;
    input_pull = 0xFFFF_FFFF;
    read_level = 1;
    read_err = 0;
    icu_calls = 0;
    icu_err = 0;
    isr_calls = 0;
    isr_event = 0xFFFF;
}

test "two switches on the pins the UM lists" {
    try std.testing.expectEqual(@as(usize, 2), switches.pins.len);
    // SW1 P009, SW2 P008.
    try std.testing.expectEqual(@as(u16, 0x0009), switches.pins[0]);
    try std.testing.expectEqual(@as(u16, 0x0008), switches.pins[1]);
}

test "SW1 is IRQ13 and SW2 is IRQ12, not renumbered by position" {
    try std.testing.expectEqual(@as(u8, 13), switches.irq_nums[0]);
    try std.testing.expectEqual(@as(u8, 12), switches.irq_nums[1]);
    try std.testing.expectEqual(@as(u16, 0x00E), switches.events[0]);
    try std.testing.expectEqual(@as(u16, 0x00D), switches.events[1]);
}

test "pinOf and readPin refuse an id past the end" {
    try std.testing.expect(switches.pinOf(2) == null);
    var pin: u16 = 0xBEEF;
    try std.testing.expect(switches.readPin(2, &pin) != 0);
    try std.testing.expectEqual(@as(u16, 0xBEEF), pin);
    try std.testing.expectEqual(@as(u32, 0), switches.readPin(0, &pin));
    try std.testing.expectEqual(@as(u16, 0x0009), pin);
}

test "init pulls the pin up, since the button shorts to ground" {
    reset();
    try std.testing.expectEqual(@as(u32, 0), switches.init(1));
    try std.testing.expectEqual(@as(u16, 0x0008), input_pin);
    try std.testing.expectEqual(@as(u32, 1), input_pull);
}

test "low reads pressed and high reads released" {
    reset();
    var pressed: u8 = 0xFF;

    read_level = 0;
    try std.testing.expectEqual(@as(u32, 0), switches.read(0, &pressed));
    try std.testing.expectEqual(@as(u8, 1), pressed);

    read_level = 1;
    try std.testing.expectEqual(@as(u32, 0), switches.read(0, &pressed));
    try std.testing.expectEqual(@as(u8, 0), pressed);
}

test "a failed read leaves the caller's byte untouched" {
    reset();
    var pressed: u8 = 0x7F;
    read_err = 0x302;
    try std.testing.expectEqual(@as(u32, 0x302), switches.read(0, &pressed));
    try std.testing.expectEqual(@as(u8, 0x7F), pressed);
}

test "attachIrq configures falling edge with the PCLKB filter on" {
    reset();
    try std.testing.expectEqual(@as(u32, 0), switches.attachIrq(0, noopHandler, null));
    try std.testing.expectEqual(@as(u32, 1), icu_calls);
    try std.testing.expectEqual(@as(u8, 13), icu_irq);
    try std.testing.expectEqual(@as(u8, 0), icu_sense);
    try std.testing.expectEqual(@as(u8, 0), icu_filter_div);
    try std.testing.expect(icu_filter_en);
}

test "attachIrq routes the matching ELC event at the default priority" {
    reset();
    var token: u32 = 7;
    try std.testing.expectEqual(@as(u32, 0), switches.attachIrq(1, noopHandler, &token));
    try std.testing.expectEqual(@as(u16, 0x00D), isr_event);
    try std.testing.expectEqual(@as(u8, 8), isr_prio);
    try std.testing.expectEqual(@as(?*anyopaque, @ptrCast(&token)), isr_ctx);
    try std.testing.expect(isr_slot_ptr_was_null);
}

test "a null callback is refused before any hardware is touched" {
    reset();
    try std.testing.expect(switches.attachIrq(0, null, null) != 0);
    try std.testing.expectEqual(@as(u32, 0), icu_calls);
    try std.testing.expectEqual(@as(u32, 0), isr_calls);
}

test "a bad switch id is refused before any hardware is touched" {
    reset();
    try std.testing.expect(switches.attachIrq(5, noopHandler, null) != 0);
    try std.testing.expectEqual(@as(u32, 0), icu_calls);
    try std.testing.expectEqual(@as(u32, 0), isr_calls);
}

test "a failed ICU configure stops short of the ISR registration" {
    reset();
    icu_err = 0x303;
    try std.testing.expectEqual(@as(u32, 0x303), switches.attachIrq(0, noopHandler, null));
    try std.testing.expectEqual(@as(u32, 1), icu_calls);
    try std.testing.expectEqual(@as(u32, 0), isr_calls);
}
