//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Pmod2 (J25) Simple-SPI bus pins on the EK-RA8D2 v1.
//!
//! P601..P604, driven from SCI0: the SPI function has no mapping on these
//! pins, so the three bus signals route to the SCI peripheral and the chip
//! select stays a plain GPIO the caller drives per transfer.

const hal = @import("hal.zig");
const vocab = @import("vocab.zig");

const Err = vocab.Err;
const Io = vocab.Io;
const Pin = vocab.Pin;
const Psel = vocab.Psel;

/// Pmod2.4 SCK (SCK0_B), P601. UM Table 19 p 27.
pub const sck = Pin.pack(6, 1);
/// Pmod2.3 CIPO (CIPO0_B), P602.
pub const cipo = Pin.pack(6, 2);
/// Pmod2.2 COPI (COPI0_B), P603.
pub const copi = Pin.pack(6, 3);
/// Pmod2.1 CS (SS0_B), P604. Held by firmware, not the peripheral.
pub const cs = Pin.pack(6, 4);

/// The three signals the SCI drives, in bus order.
pub const bus = [_]u16{ sck, cipo, copi };

/// Route the bus to SCI0 and park CS high (deasserted).
///
/// Run before `ra8_sci_spi_init` on the Pmod2 channel.
pub fn init() u32 {
    for (bus) |pin| {
        const err = hal.ra8_pfs_route_peripheral(pin, Psel.sci_async, "ra8_board.pmod2");
        if (err != Err.ok) return err;
    }
    return hal.ra8_gpio_output_init(cs, Io.level_high);
}

/// Drive CS. Asserted is low, the usual SPI convention.
pub fn csSet(asserted: bool) u32 {
    const level = if (asserted) Io.level_low else Io.level_high;
    return hal.ra8_gpio_write(cs, level);
}
