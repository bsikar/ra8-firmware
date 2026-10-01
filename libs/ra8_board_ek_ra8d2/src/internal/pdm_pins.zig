//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Which package pins carry the SPH0690 microphone clock and data.

const hal = @import("hal.zig");
const vocab = @import("vocab.zig");

const Err = vocab.Err;
const Pin = vocab.Pin;

/// A routed PDM pin and the owner string the PFS arbiter records, so a
/// double-claim names the board function rather than a bare pin number.
pub const Route = struct {
    pin: u16,
    owner: [*:0]const u8,
};

pub const clk: u16 = Pin.pack(8, 12);
pub const dat: u16 = Pin.pack(5, 2);

pub const routes = [_]Route{
    .{ .pin = clk, .owner = "board.pdm.clk" },
    .{ .pin = dat, .owner = "board.pdm.dat" },
};

/// Route both pins to the PDM-IF. Stops at the first refusal so a conflicting
/// route is reported against the pin that actually clashed.
pub fn routeAll() u32 {
    for (routes) |route| {
        const err = hal.ra8_pfs_route_peripheral(route.pin, vocab.Psel.pdm, route.owner);
        if (err != Err.ok) return err;
    }
    return Err.ok;
}
