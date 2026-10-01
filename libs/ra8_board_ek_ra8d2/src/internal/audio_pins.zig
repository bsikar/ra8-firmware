//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The six DA7212 pins: four SSIE0 signals and the IIC control pair.
//! UM Table 32 p 38.
//!
//! MCLK on PD06 is deliberately absent. It stays a GPIO so the application
//! picks SSIE EXTAL or a CGC clock-out explicitly.

const hal = @import("hal.zig");
const vocab = @import("vocab.zig");

const Err = vocab.Err;
const Pin = vocab.Pin;
const Psel = vocab.Psel;

pub const Route = struct {
    pin: u16,
    psel: u32,
    owner: [*:0]const u8,
};

pub const bclk: u16 = Pin.pack(4, 3);
pub const wclk: u16 = Pin.pack(4, 4);
pub const datin: u16 = Pin.pack(4, 5);
pub const datout: u16 = Pin.pack(4, 6);
pub const i2c_sda: u16 = Pin.pack(5, 11);
pub const i2c_scl: u16 = Pin.pack(5, 12);

pub const routes = [_]Route{
    .{ .pin = bclk, .psel = Psel.ssie, .owner = "ra8_board.audio.bclk" },
    .{ .pin = wclk, .psel = Psel.ssie, .owner = "ra8_board.audio.wclk" },
    .{ .pin = datin, .psel = Psel.ssie, .owner = "ra8_board.audio.datin" },
    .{ .pin = datout, .psel = Psel.ssie, .owner = "ra8_board.audio.datout" },
    .{ .pin = i2c_sda, .psel = Psel.iic, .owner = "ra8_board.audio.i2c.sda" },
    .{ .pin = i2c_scl, .psel = Psel.iic, .owner = "ra8_board.audio.i2c.scl" },
};

/// Route all six, stopping at the first refusal.
pub fn routeAll() u32 {
    for (routes) |route| {
        const err = hal.ra8_pfs_route_peripheral(route.pin, route.psel, route.owner);
        if (err != Err.ok) return err;
    }
    return Err.ok;
}
