//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Whether the link will take one Ethernet frame for transmission.
//!
//! The three refusals have nothing to do with the frame's contents: the link
//! has to be open, the length has to fit a single transaction, and the one
//! transmit slot has to be free. Copying the frame into that slot and
//! clocking it out stays in C; this is the verdict it asks for first, and it
//! is the same shape as `rpc_wait.issuable`, which guards the other sender.

const frame = @import("frame.zig");

/// Why one frame was refused.
pub const Refusal = error{
    NotInitialized,
    InvalidSize,
    Busy,
};

/// Why one H4 HCI packet was refused.
pub const HciRefusal = error{
    NotInitialized,
    InvalidSize,
    InvalidArg,
    Busy,
};

/// Lengths one frame is held to.
pub const Bound = struct {
    /// Shortest frame worth clocking out; an empty one carries nothing.
    pub const min: u16 = 1;
    /// Longest frame one transaction can carry.
    pub const max: u16 = frame.Frame.max_payload;
};

/// Lengths one H4 HCI packet (`[type][body...]`) is held to.
pub const HciBound = struct {
    /// Indicator octet plus at least one body octet.
    pub const min: u16 = 2;
    /// Indicator rides in header octet 11 (`hci_pkt_type`), body in payload.
    pub const max: u16 = frame.Frame.max_payload + 1;
};

/// Valid Bluetooth Core Vol 4 Part A H4 host-to-controller indicators.
pub fn isValidHciPktType(pkt_type: u8) bool {
    return switch (pkt_type) {
        0x01, 0x02, 0x03, 0x05 => true,
        else => false,
    };
}

/// May this frame go out right now?
///
/// The order is the order the caller reports: a closed link is refused
/// before its length is read, and a length is refused before the slot is
/// looked at, so a caller passing a bad length to a closed link hears about
/// the link.
pub fn admit(open: bool, len: u16, tx_len: u16) Refusal!void {
    if (!open) return Refusal.NotInitialized;
    if (len < Bound.min or len > Bound.max) return Refusal.InvalidSize;
    if (tx_len != 0) return Refusal.Busy;
}

/// May this H4 HCI packet go out right now?
///
/// Checks open state before packet bounds, bounds before the H4 type
/// indicator, and H4 indicator before staged-slot busy state.
pub fn admitHci(open: bool, len: u16, pkt_type: u8, tx_len: u16) HciRefusal!void {
    if (!open) return HciRefusal.NotInitialized;
    if (len < HciBound.min or len > HciBound.max) return HciRefusal.InvalidSize;
    if (!isValidHciPktType(pkt_type)) return HciRefusal.InvalidArg;
    if (tx_len != 0) return HciRefusal.Busy;
}
