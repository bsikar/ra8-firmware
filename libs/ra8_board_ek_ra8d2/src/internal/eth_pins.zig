//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The EK-RA8D2's Ethernet pin map: which package pins carry RGMII, which of
//! them need middle drive strength, and which one is the PHY reset line that
//! must stay a GPIO. UM Table 26 p 33.

const vocab = @import("vocab.zig");

const Pin = vocab.Pin;

/// A routed Ethernet pin and the owner string the PFS arbiter records, so a
/// double-claim names the board function rather than a bare pin number.
pub const Route = struct {
    pin: u16,
    owner: [*:0]const u8,
};

/// The PHY reset line. Driven as a GPIO, never routed to RGMII, which is why
/// `routes` carries it and `routeAll` skips it.
pub const rstn: u16 = Pin.pack(7, 8);

/// Every Ethernet pin on the board, reset line included.
pub const routes = [_]Route{
    .{ .pin = Pin.pack(1, 7), .owner = "ra8_board.eth.mdint" },
    .{ .pin = Pin.pack(4, 15), .owner = "ra8_board.eth.mdc" },
    .{ .pin = Pin.pack(4, 14), .owner = "ra8_board.eth.mdio" },
    .{ .pin = Pin.pack(3, 7), .owner = "ra8_board.eth.txd0" },
    .{ .pin = Pin.pack(3, 6), .owner = "ra8_board.eth.txd1" },
    .{ .pin = Pin.pack(3, 5), .owner = "ra8_board.eth.txd2" },
    .{ .pin = Pin.pack(3, 4), .owner = "ra8_board.eth.txd3" },
    .{ .pin = Pin.pack(3, 10), .owner = "ra8_board.eth.tx_ctl" },
    .{ .pin = Pin.pack(3, 9), .owner = "ra8_board.eth.tx_clk" },
    .{ .pin = Pin.pack(9, 6), .owner = "ra8_board.eth.rxd0" },
    .{ .pin = Pin.pack(9, 7), .owner = "ra8_board.eth.rxd1" },
    .{ .pin = Pin.pack(9, 8), .owner = "ra8_board.eth.rxd2" },
    .{ .pin = Pin.pack(9, 9), .owner = "ra8_board.eth.rxd3" },
    .{ .pin = Pin.pack(2, 6), .owner = "ra8_board.eth.rx_ctl" },
    .{ .pin = Pin.pack(9, 5), .owner = "ra8_board.eth.rx_clk" },
    .{ .pin = rstn, .owner = "ra8_board.eth.rstn" },
};

/// The six RGMII transmit pins, which need DSCR = 01b middle drive strength.
/// HUM Ch 20.2.6 "PmnPFS" p 845.
pub const tx_pins = [_]u16{
    Pin.pack(3, 7),
    Pin.pack(3, 6),
    Pin.pack(3, 5),
    Pin.pack(3, 4),
    Pin.pack(3, 10),
    Pin.pack(3, 9),
};
