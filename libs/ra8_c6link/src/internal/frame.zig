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
    /// `ESP_PRIV_IF`, the privileged control interface.
    pub const priv: u8 = 5;
};

/// The one privileged event whose checksum defect is tolerated (RA8FW-276).
///
/// The ESP32-C6 running esp-hosted-mcu 2.12.11 queues an `ESP_PRIV_EVENT_INIT`
/// frame at every boot, stamps a non-zero `if_num` into the header's first
/// octet and only then finishes the frame, but it computed the checksum while
/// that nibble was still zero. The transmitted checksum is therefore short of
/// the received span's sum by exactly `if_num << 4`. `classify` accepts that
/// one shape and nothing else.
pub const PrivInit = struct {
    /// `ESP_PRIV_EVENT_INIT`, the event-type octet that opens the payload.
    pub const event: u8 = 0x22;
    /// Octets before the first TLV: the event type, then the TLV byte count.
    pub const hdr: u16 = 2;
    /// Octets opening each TLV: tag identifier, then value length.
    pub const tlv_hdr: usize = 2;
    /// Bit position of `if_num` inside the header's first octet.
    pub const ifnum_shift: u4 = 4;
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
    /// The co-processor's bootup `ESP_PRIV_EVENT_INIT` frame, whose checksum
    /// was computed with `if_num` zero and which verified under exactly that
    /// reading (RA8FW-276). The payload is real; the `if_num` nibble alone is
    /// not covered by the integrity check.
    priv_init: View,
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
/// Sets `pkt_type` in byte 11 before computing the checksum (`hci_pkt_type`
/// rides here for `ESP_HCI_IF` frames). Returns false, leaving `tx`
/// untouched, when the payload could not fit.
pub fn sealPkt(tx: []u8, if_type: u8, if_num: u8, pkt_type: u8, len: u16) bool {
    if (len > Frame.max_payload) return false;
    const span = Frame.header_bytes + len;

    @memset(tx[span..], 0);
    @memset(tx[0..Frame.header_bytes], 0);
    tx[Hdr.iface] = (if_type & Hdr.nibble) | ((if_num & Hdr.nibble) << 4);
    std.mem.writeInt(u16, tx[Hdr.len..][0..2], len, .little);
    std.mem.writeInt(u16, tx[Hdr.offset..][0..2], Frame.header_bytes, .little);
    std.mem.writeInt(u16, tx[Hdr.seq_num..][0..2], Hdr.seq, .little);
    tx[Hdr.pkt_type] = pkt_type;

    const checksum = sum(tx[0..span]);
    std.mem.writeInt(u16, tx[Hdr.checksum..][0..2], checksum, .little);
    return true;
}

/// Seal a transaction whose payload is already staged at `Frame.header_bytes`.
///
/// Returns false, leaving `tx` untouched, when the payload could not fit.
pub fn seal(tx: []u8, if_type: u8, if_num: u8, len: u16) bool {
    return sealPkt(tx, if_type, if_num, 0, len);
}

/// Describe where a verified frame's payload is.
fn viewOf(rx: []const u8, offset: u16, len: u16) View {
    return .{
        .offset = offset,
        .len = len,
        .if_type = rx[Hdr.iface] & Hdr.nibble,
        .if_num = rx[Hdr.iface] >> 4,
    };
}

/// True when `payload` opens with `ESP_PRIV_EVENT_INIT` and its declared TLV
/// span fits inside `payload` without truncation.
pub fn isPrivInitPayload(payload: []const u8) bool {
    if (payload.len < PrivInit.hdr) return false;
    if (payload[0] != PrivInit.event) return false;
    const end: usize = @as(usize, payload[1]) + PrivInit.hdr;
    if (end > payload.len) return false;

    var at: usize = PrivInit.hdr;
    while (at < end) {
        if (at + PrivInit.tlv_hdr > end) return false;
        const tlv_len: usize = payload[at + 1];
        if (at + PrivInit.tlv_hdr + tlv_len > end) return false;
        at += PrivInit.tlv_hdr + tlv_len;
    }
    return true;
}

/// True when a checksum mismatch is exactly the bootup INIT event's known
/// `if_num` defect (RA8FW-276), and nothing else.
///
/// Every condition has to hold, so no other privileged frame, no other event
/// and no other corruption is excused:
///
/// - the frame is on `ESP_PRIV_IF` and carries a non-zero `if_num`;
/// - `isPrivInitPayload` accepts the payload (`ESP_PRIV_EVENT_INIT` with a
///   bounded, non-truncated TLV list);
/// - the transmitted checksum equals the received span's sum taken with the
///   `if_num` nibble zero, which is `actual - (if_num << 4)`.
///
/// Every payload octet and the `if_type` nibble stay under the integrity check
/// on this path. Only the `if_num` nibble is taken on trust, and nothing
/// consumes it for a privileged frame.
fn privInitDefect(rx: []const u8, offset: u16, len: u16, actual: u16, claimed: u16) bool {
    const if_type = rx[Hdr.iface] & Hdr.nibble;
    const if_num = rx[Hdr.iface] >> PrivInit.ifnum_shift;
    if (if_type != Iface.priv or if_num == 0) return false;
    if (!isPrivInitPayload(rx[offset..][0..len])) return false;

    const shortfall = @as(u16, if_num) << PrivInit.ifnum_shift;
    return actual -% shortfall == claimed;
}

/// Decide what a received transaction is, without modifying it.
pub fn classify(rx: []const u8) Class {
    const len = std.mem.readInt(u16, rx[Hdr.len..][0..2], .little);
    if (len == 0) return .idle;

    const offset = std.mem.readInt(u16, rx[Hdr.offset..][0..2], .little);
    if (offset != Frame.header_bytes or len > Frame.max_payload) return .malformed;

    const claimed = std.mem.readInt(u16, rx[Hdr.checksum..][0..2], .little);
    const actual = sumAsZeroed(rx, offset + len);
    if (actual == claimed) return .{ .data = viewOf(rx, offset, len) };
    if (privInitDefect(rx, offset, len, actual, claimed)) {
        return .{ .priv_init = viewOf(rx, offset, len) };
    }
    return .bad_checksum;
}
