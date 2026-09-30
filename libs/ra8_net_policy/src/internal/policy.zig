//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The decisions the guard exists to make: what a peer address literal is,
//! whether that class may be fetched, and whether a download has outgrown its
//! cap. Every answer here is a pure function of its arguments.

const root = @import("root.zig");
const v4 = @import("v4.zig");
const v6 = @import("v6.zig");

/// Re-exported so a caller of the policy never reaches past it for the type.
pub const AddrClass = root.AddrClass;

/// Classify an address literal, IPv4 first, then IPv6.
///
/// Anything that does not parse is `unknown`, which is never fetchable, so a
/// hostname or a malformed literal can never be mistaken for a public peer.
pub fn classifyIp(ip: []const u8) AddrClass {
    if (ip.len == 0) return .unknown;
    if (v4.parse(ip)) |octets| return v4.classify(octets);
    if (v6.parse(ip)) |bytes| return v6.classify(bytes);
    return .unknown;
}

/// Whether an address of this class may be fetched.
///
/// Public always may. Unknown never may, whatever the caller opted into.
/// Loopback, private and link-local ride the caller's explicit opt-in.
pub fn fetchable(cls: AddrClass, allow_private: bool) bool {
    if (cls == .public) return true;
    if (cls == .unknown) return false;
    return allow_private;
}

/// Whether `add` more bytes would carry `have` past `cap`.
///
/// A zero cap means no cap. The comparison is rearranged so the sum is never
/// formed, which keeps a hostile content-length from wrapping past the check.
pub fn sizeExceeds(have: u64, add: u64, cap: u64) bool {
    if (cap == 0) return false;
    if (have > cap) return true;
    return add > (cap - have);
}
