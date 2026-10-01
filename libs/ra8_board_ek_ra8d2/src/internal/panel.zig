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

/// BLEN is active high and already an output after `powerOn`.
pub fn backlight(on: bool) u32 {
    return hal.ra8_gpio_write(Panel.blen, if (on) Level.high else Level.low);
}
