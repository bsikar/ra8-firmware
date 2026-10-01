//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The Arduino header pins as plain GPIO. UM Table 20 p 28.
//!
//! A pin value here is already a packed (port, index), so these are thin
//! passes to the GPIO layer; the board's contribution is the mode vocabulary.

const hal = @import("hal.zig");
const vocab = @import("vocab.zig");

const Arduino = vocab.Arduino;
const Err = vocab.Err;
const Io = vocab.Io;
const Level = vocab.Level;

pub fn pinInit(pin: u16, mode: u8) u32 {
    return switch (mode) {
        Arduino.mode_input => hal.ra8_gpio_input_init(pin, Io.pull_none),
        Arduino.mode_input_pullup => hal.ra8_gpio_input_init(pin, Io.pull_up),
        Arduino.mode_output => hal.ra8_gpio_output_init(pin, Level.low),
        else => Err.invalid_arg,
    };
}

pub fn write(pin: u16, level: u32) u32 {
    return hal.ra8_gpio_write(pin, level);
}

pub fn read(pin: u16, out_level: *u32) u32 {
    return hal.ra8_gpio_read(pin, out_level);
}
