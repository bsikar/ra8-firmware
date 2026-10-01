//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The SDHI0 bus pins. The EK-RA8D2 v1 has no on-board micro-SD socket, so
//! these are for an adapter on the headers.

const hal = @import("hal.zig");
const vocab = @import("vocab.zig");

const Err = vocab.Err;
const Pin = vocab.Pin;
const Psel = vocab.Psel;

/// CMD, CLK, DAT0..DAT3, WP, CD: P400 through P407, in bus order.
pub const bus = [_]u16{
    Pin.pack(4, 0),
    Pin.pack(4, 1),
    Pin.pack(4, 2),
    Pin.pack(4, 3),
    Pin.pack(4, 4),
    Pin.pack(4, 5),
    Pin.pack(4, 6),
    Pin.pack(4, 7),
};

pub fn init() u32 {
    for (bus) |pin| {
        const err = hal.ra8_pfs_route_peripheral(pin, Psel.sdhi, "ra8_board.sdhi");
        if (err != Err.ok) return err;
    }
    return Err.ok;
}
