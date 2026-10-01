//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! How much of one incoming protobuf field may be taken into a fixed
//! destination.
//!
//! Nothing here copies anything. The decoder's fields are owned by the
//! protobuf-c arena and the destinations are fixed-capacity members of the
//! public event and record types, so the copy itself stays at the C
//! membrane. Only the two bounds that decide it live here: how many octets
//! of a text field fit a terminated buffer, and whether a binary field
//! carries one hardware address.

/// Sizes an incoming field is judged against.
pub const Bound = struct {
    /// Octets in one hardware address, as the public header fixes it.
    pub const mac_octets: usize = 6;
};

/// Octets of a text field that fit a destination of `cap` octets.
///
/// The destination is always terminated, so one octet of its capacity is
/// spent on the terminator and never on the field. A field longer than the
/// room left is truncated rather than refused: these are display strings
/// from the peer, and a short SSID is more useful than a dropped event. A
/// zero capacity has no room for even the terminator, so nothing is taken.
pub fn strTake(src_len: usize, cap: u8) usize {
    if (cap == 0) return 0;
    const room: usize = @as(usize, cap) - 1;
    return @min(src_len, room);
}

/// Does a binary field carry exactly one hardware address?
///
/// Exact, not bounded: a short field would leave octets of the destination
/// unwritten and a long one is not an address at all, and in both cases the
/// peer has said something this library does not understand.
pub fn macAcceptable(src_len: usize) bool {
    return src_len == Bound.mac_octets;
}
