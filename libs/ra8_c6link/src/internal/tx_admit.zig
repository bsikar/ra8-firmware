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

/// Lengths one frame is held to.
pub const Bound = struct {
    /// Shortest frame worth clocking out; an empty one carries nothing.
    pub const min: u16 = 1;
    /// Longest frame one transaction can carry.
    pub const max: u16 = frame.Frame.max_payload;
};

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
