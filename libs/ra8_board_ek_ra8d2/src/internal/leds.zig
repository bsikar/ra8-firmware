//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The three user LEDs. UM Table 24 p 31.

const hal = @import("hal.zig");
const vocab = @import("vocab.zig");

const Err = vocab.Err;
const Level = vocab.Level;
const Pin = vocab.Pin;

/// LED1 blue (P600, jumper E27), LED2 green (P303, E26), LED3 red (PA07, E28).
pub const pins = [_]u16{
    Pin.pack(6, 0),
    Pin.pack(3, 3),
    Pin.pack(10, 7),
};

pub fn pinOf(led: u8) ?u16 {
    if (led >= pins.len) return null;
    return pins[led];
}

pub fn readPin(led: u8, out_pin: *u16) u32 {
    const pin = pinOf(led) orelse return Err.invalid_arg;
    out_pin.* = pin;
    return Err.ok;
}

pub fn init(led: u8) u32 {
    const pin = pinOf(led) orelse return Err.invalid_arg;
    return hal.ra8_gpio_output_init(pin, Level.low);
}

pub fn on(led: u8) u32 {
    const pin = pinOf(led) orelse return Err.invalid_arg;
    return hal.ra8_gpio_write(pin, Level.high);
}

pub fn off(led: u8) u32 {
    const pin = pinOf(led) orelse return Err.invalid_arg;
    return hal.ra8_gpio_write(pin, Level.low);
}

pub fn toggle(led: u8) u32 {
    const pin = pinOf(led) orelse return Err.invalid_arg;
    return hal.ra8_gpio_toggle(pin);
}
