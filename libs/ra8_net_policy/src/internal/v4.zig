//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! IPv4 literal parsing and address classification. Only a canonical dotted
//! quad parses: a leading zero, an over-long digit run, an out-of-range octet,
//! a missing or extra dot, and any trailing byte are all refused.

const ascii = @import("ascii.zig");
const root = @import("root.zig");

const AddrClass = root.AddrClass;
const limits = root.limits;

/// IPv4 range boundaries that mark non-public address space.
const range = struct {
    pub const zero_net: u8 = 0; // 0.0.0.0/8 "this network"
    pub const private_a: u8 = 10; // 10.0.0.0/8
    pub const cgnat: u8 = 100; // 100.64.0.0/10 (RFC6598)
    pub const cgnat_lo: u8 = 64;
    pub const cgnat_hi: u8 = 127;
    pub const loopback_net: u8 = 127; // 127.0.0.0/8
    pub const linklocal: u8 = 169; // 169.254.0.0/16
    pub const linklocal_2: u8 = 254;
    pub const private_b: u8 = 172; // 172.16.0.0/12
    pub const private_b_lo: u8 = 16;
    pub const private_b_hi: u8 = 31;
    pub const private_c: u8 = 192; // 192.168.0.0/16
    pub const private_c_2: u8 = 168;
    pub const multicast_min: u8 = 224; // 224.0.0.0/4 and up
};

/// Parse a canonical dotted-quad literal spanning the whole of `text`.
pub fn parse(text: []const u8) ?[limits.v4_bytes]u8 {
    var out: [limits.v4_bytes]u8 = .{0} ** limits.v4_bytes;
    var at: usize = 0;
    for (0..limits.v4_bytes) |octet| {
        if (octet != 0) {
            if (ascii.byteAt(text, at) != '.') return null;
            at += 1;
        }
        if (!isDigit(ascii.byteAt(text, at))) return null;
        if (ascii.byteAt(text, at) == '0' and isDigit(ascii.byteAt(text, at + 1))) {
            return null; // a leading zero is not a canonical octet
        }
        out[octet] = readOctet(text, &at) orelse return null;
    }
    return if (at == text.len) out else null;
}

/// Classify four parsed octets.
///
/// The ranges are applied in the order that keeps each test independent of the
/// ones before it, so the first match wins and none of them overlap.
pub fn classify(o: [limits.v4_bytes]u8) AddrClass {
    if (o[0] == range.zero_net) return .unknown;
    if (o[0] == range.loopback_net) return .loopback;
    if (o[0] >= range.multicast_min) return .unknown;
    if (o[0] == range.linklocal) {
        return if (o[1] == range.linklocal_2) .linklocal else .public;
    }
    if (isPrivate(o[0], o[1])) return .private;
    return .public;
}

fn isDigit(c: u8) bool {
    return (c >= '0') and (c <= '9');
}

fn readOctet(text: []const u8, at: *usize) ?u8 {
    var value: u32 = 0;
    var digits: usize = 0;
    while (isDigit(ascii.byteAt(text, at.*))) {
        if (digits == limits.v4_digits_max) return null;
        value = (value * 10) + (ascii.byteAt(text, at.*) - '0');
        digits += 1;
        at.* += 1;
    }
    if (value > limits.v4_octet_max) return null;
    return @intCast(value);
}

fn isPrivate(o0: u8, o1: u8) bool {
    if (o0 == range.private_a) return true;
    if (o0 == range.private_b) return (o1 >= range.private_b_lo) and (o1 <= range.private_b_hi);
    if (o0 == range.private_c) return o1 == range.private_c_2;
    if (o0 == range.cgnat) return (o1 >= range.cgnat_lo) and (o1 <= range.cgnat_hi);
    return false;
}
