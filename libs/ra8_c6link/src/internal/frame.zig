//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The twelve-byte esp-hosted payload header: build it, believe it or not.
//!
//! Every transaction on this link carries the same header in front of whatever
//! it is transporting: an interface nibble pair, a length, an offset, a
//! checksum and a sequence number. This file is the only place that reads or
//! writes it.
//!
//! The header is addressed by named octet offsets rather than by casting the
//! buffer to a struct. The wire layout is little-endian regardless of what the
//! host is, so the reads and writes say so, and a frame can be classified
//! without the aliasing question a pointer cast would raise.

const std = @import("std");

/// Transaction geometry, restated by the public C header so consumers need no
/// esp-hosted include path. `ra8_c6link_internal.h` carries the static assertions that
/// tie these to the vendored declaration.
pub const Frame = struct {
    /// Octets clocked in one transaction, `ESP_TRANSPORT_SPI_MAX_BUF_SIZE`.
    pub const bytes: u16 = 1600;
    /// `sizeof(struct esp_payload_header)`.
    pub const header_bytes: u16 = 12;
    /// What is left of a transaction once the header has its share.
    pub const max_payload: u16 = bytes - header_bytes;
};

/// Octet offsets of the header's fields, and the two field-level constants
/// reading them needs.
pub const Hdr = struct {
    /// `if_type` in the low nibble, `if_num` in the high nibble.
    pub const iface: u16 = 0;
    pub const flags: u16 = 1;
    pub const len: u16 = 2;
    pub const offset: u16 = 4;
    pub const checksum: u16 = 6;
    pub const seq_num: u16 = 8;
    pub const throttle: u16 = 10;
    pub const pkt_type: u16 = 11;

    /// Widest value either interface field can hold.
    pub const nibble: u8 = 0x0F;
    /// Octets the checksum field occupies, which the verifier subtracts.
    pub const checksum_bytes: u16 = 2;

    /// The sequence number transmitted, always zero.
    ///
    /// Upstream increments it, but the bench run that proved this link
    /// answered a request whose sequence number was zero and nothing in the
    /// reply depended on it. Changing a proven variable for cosmetic parity is
    /// how a working link acquires an unexplained failure.
    pub const seq: u16 = 0;
};

/// Interface identifiers from the vendored `esp_hosted_interface.h`.
pub const Iface = struct {
    /// `ESP_MAX_IF`, the out-of-range value the idle filler frame carries.
    pub const max: u8 = 8;
};

/// What a received transaction turned out to be.
pub const Class = union(enum) {
    /// A well-formed frame whose checksum verified; the payload is real.
    data: View,
    /// The co-processor's filler frame: zero length, and legitimately
    /// `offset = 0`, which is not a defect.
    idle,
    /// The offset was not the header size, or the length did not fit.
    malformed,
    /// The recomputed checksum disagreed with the transmitted one.
    bad_checksum,
};

/// Where the payload of a classified frame is, and what it claims to be.
///
/// Holds offsets rather than a slice so it cannot outlive the buffer it
/// describes, which is the same reason the C held offsets.
pub const View = struct {
    offset: u16,
    len: u16,
    if_type: u8,
    if_num: u8,
};

/// Sum every octet of a span, 16-bit and wrapping.
///
/// Matches the accumulator in the vendored `compute_checksum()`.
fn sum(span: []const u8) u16 {
    var total: u16 = 0;
    for (span) |byte| total +%= byte;
    return total;
}

/// Recompute a frame's checksum as if its checksum field were zero.
///
/// Subtracting the transmitted checksum octets from the sum of the whole span
/// is arithmetically identical to upstream's zero-the-field method and leaves
/// the received buffer untouched, which is what lets a frame be classified
/// without a write.
fn sumAsZeroed(frame: []const u8, span: u16) u16 {
    var total = sum(frame[0..span]);
    for (0..Hdr.checksum_bytes) |i| total -%= frame[Hdr.checksum + i];
    return total;
}

/// Write the idle filler frame: an all-zero transaction addressed to no
/// interface at all.
pub fn filler(tx: []u8) void {
    @memset(tx, 0);
    tx[Hdr.iface] = Iface.max & Hdr.nibble;
}

/// Seal a transaction whose payload is already staged at `Frame.header_bytes`.
///
/// Returns false, leaving `tx` untouched, when the payload could not fit.
pub fn seal(tx: []u8, if_type: u8, if_num: u8, len: u16) bool {
    if (len > Frame.max_payload) return false;
    const span = Frame.header_bytes + len;

    @memset(tx[span..], 0);
    @memset(tx[0..Frame.header_bytes], 0);
    tx[Hdr.iface] = (if_type & Hdr.nibble) | ((if_num & Hdr.nibble) << 4);
    std.mem.writeInt(u16, tx[Hdr.len..][0..2], len, .little);
    std.mem.writeInt(u16, tx[Hdr.offset..][0..2], Frame.header_bytes, .little);
    std.mem.writeInt(u16, tx[Hdr.seq_num..][0..2], Hdr.seq, .little);

    const checksum = sum(tx[0..span]);
    std.mem.writeInt(u16, tx[Hdr.checksum..][0..2], checksum, .little);
    return true;
}

/// Decide what a received transaction is, without modifying it.
pub fn classify(rx: []const u8) Class {
    const len = std.mem.readInt(u16, rx[Hdr.len..][0..2], .little);
    if (len == 0) return .idle;

    const offset = std.mem.readInt(u16, rx[Hdr.offset..][0..2], .little);
    if (offset != Frame.header_bytes or len > Frame.max_payload) return .malformed;

    const claimed = std.mem.readInt(u16, rx[Hdr.checksum..][0..2], .little);
    if (sumAsZeroed(rx, offset + len) != claimed) return .bad_checksum;

    return .{ .data = .{
        .offset = offset,
        .len = len,
        .if_type = rx[Hdr.iface] & Hdr.nibble,
        .if_num = rx[Hdr.iface] >> 4,
    } };
}
