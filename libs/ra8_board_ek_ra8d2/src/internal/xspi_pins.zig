//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The Octo-SPI flash bus: the IS25LX512M reset strap and the twelve OCTA
//! pins. UM Table 29 p 35.

const hal = @import("hal.zig");
const vocab = @import("vocab.zig");

const Err = vocab.Err;
const Level = vocab.Level;
const Pin = vocab.Pin;
const Psel = vocab.Psel;
const Xspi = vocab.Xspi;

/// CS, CK, DQS, DQ0..DQ7. RESET_L is deliberately not here: it is an
/// active-low GPIO strap, not a peripheral function.
pub const bus = [_]u16{
    Pin.pack(1, 4),
    Pin.pack(8, 8),
    Pin.pack(8, 1),
    Pin.pack(1, 0),
    Pin.pack(8, 3),
    Pin.pack(1, 3),
    Pin.pack(1, 1),
    Pin.pack(1, 2),
    Pin.pack(8, 0),
    Pin.pack(8, 2),
    Pin.pack(8, 4),
};

pub const reset_pin: u16 = Pin.pack(1, 6);

/// Reset strap first, while the pin is still GPIO, then the bus.
///
/// The post-release wait is tPUW from the datasheet's power-up window, not
/// the much shorter reset recovery: from a cold boot the controller could
/// otherwise clock RDID before the internal regulator was stable, the part
/// would silently NAK, and CMDCMP would never assert.
pub fn init() u32 {
    var err = hal.ra8_gpio_output_init(reset_pin, Level.low);
    if (err != Err.ok) return err;
    hal.ra8_delay_ms(Xspi.reset_low_ms);

    err = hal.ra8_gpio_write(reset_pin, Level.high);
    if (err != Err.ok) return err;
    hal.ra8_delay_ms(Xspi.reset_high_ms);

    for (bus) |pin| {
        err = hal.ra8_pfs_route_peripheral(pin, Psel.qspi, "ra8_board.xspi");
        if (err != Err.ok) return err;
    }
    return Err.ok;
}
