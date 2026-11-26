//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Where one well-formed received frame's payload goes.
//!
//! The co-processor multiplexes three unrelated conversations onto one SPI
//! link and tells them apart by an interface number in the frame header.
//! Control-plane frames belong to the RPC decoder, station and access-point
//! frames to the Ethernet receive callback, and everything else is counted
//! and dropped. `ra8_c6link_dispatch_abi.zig` reads the frame and hands the
//! payload over; this is the decision in the middle of it.

/// Interface numbers as the vendored esp-hosted header orders them.
///
/// `esp_hosted_interface.h` declares them as an unnamed enum, so the host
/// side only ever sees the byte. These names mirror that order so a reader
/// can check the two against each other without decoding ordinals.
pub const If = struct {
    pub const invalid: u8 = 0;
    pub const sta: u8 = 1;
    pub const ap: u8 = 2;
    pub const serial: u8 = 3;
    pub const hci: u8 = 4;
    pub const privileged: u8 = 5;
    pub const diagnostic: u8 = 6;
    pub const ethernet: u8 = 7;
    pub const max: u8 = 8;
};

/// The one consumer a frame is offered to.
///
/// `priv` handles `ESP_PRIV_IF` frames: validated `ESP_PRIV_EVENT_INIT`
/// boot announcements are emitted as `k_ra8_c6link_event_boot` (RA8FW-276),
/// while any non-INIT payload falls back to `unrouted` counting.
pub const Route = enum(u8) {
    rpc = 0,
    ethernet = 1,
    counted = 2,
    hci = 3,
    priv = 4,
};

/// Which consumer this interface number belongs to.
pub fn routeFor(if_type: u8) Route {
    return switch (if_type) {
        If.serial => .rpc,
        If.sta, If.ap => .ethernet,
        If.hci => .hci,
        If.privileged => .priv,
        else => .counted,
    };
}
