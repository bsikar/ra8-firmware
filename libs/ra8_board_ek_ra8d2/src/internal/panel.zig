//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The J1 panel's reset strap and backlight enable.

const hal = @import("hal.zig");
const vocab = @import("vocab.zig");

const Err = vocab.Err;
const Level = vocab.Level;
const Panel = vocab.Panel;

/// RESET_L low for 50 ms, high for another 50 ms so the panel's own power-on
/// reset finishes, then BLEN asserted.
pub fn powerOn() u32 {
    var err = hal.ra8_gpio_output_init(Panel.reset_l, Level.low);
    if (err != Err.ok) return err;
    hal.ra8_delay_ms(Panel.reset_pulse_ms);

    err = hal.ra8_gpio_write(Panel.reset_l, Level.high);
    if (err != Err.ok) return err;
    hal.ra8_delay_ms(Panel.reset_pulse_ms);

    return hal.ra8_gpio_output_init(Panel.blen, Level.high);
}

/// Capabilities of the onboard J1 panel backlight control (`ra8_board_backlight_caps_t`).
pub const BacklightCaps = extern struct {
    min_pct: u8,
    max_pct: u8,
    supports_pwm: bool,
};

/// Initialise BLEN (P514) as GPIO output without asserting RESET_L.
pub fn backlightInit(initial_on: bool) u16 {
    return hal.ra8_gpio_output_init(
        Panel.blen,
        if (initial_on) Level.high else Level.low,
    );
}

/// Report EK-RA8D2 onboard backlight capabilities (binary GPIO on/off only).
pub fn backlightCaps(out_caps: ?*BacklightCaps) u16 {
    const dst = out_caps orelse return @intCast(Err.null_ptr);
    dst.* = .{
        .min_pct = 0,
        .max_pct = 100,
        .supports_pwm = false,
    };
    return @intCast(Err.ok);
}

/// Set onboard brightness percent; 0 and 100 map to BLEN low/high, while 1..99
/// fail closed because EK-RA8D2 J1 BLEN (P514) has no GPT PWM route.
pub fn backlightBrightness(percent: u8) u16 {
    if (percent > 100) return @intCast(Err.invalid_arg);
    if (percent == 0) return @intCast(backlight(false));
    if (percent == 100) return @intCast(backlight(true));
    return @intCast(Err.not_supported);
}

/// BLEN is active high and already an output after `powerOn`.
pub fn backlight(on: bool) u32 {
    return hal.ra8_gpio_write(Panel.blen, if (on) Level.high else Level.low);
}
