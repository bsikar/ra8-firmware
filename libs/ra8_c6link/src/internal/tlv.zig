//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The two-tag envelope the co-processor's serial endpoint speaks: a tag
//! naming the endpoint, then a tag introducing the protobuf data.
//!
//! Upstream builds and parses these in `compose_tlv()` and `parse_tlv()` in
//! `drivers/virtual_serial_if/serial_if.c`, which this tree does not compile:
//! its transmit path expands `HOSTED_CALLOC`, whose failure arm is a `goto` to
//! a caller-supplied label, and NASA Power of 10 Rule 1 forbids that. The
//! envelope is trivial, so it is restated here rather than dragged in with a
//! rule violation attached.

const vocab = @import("vocab.zig");

const Ep = vocab.Ep;
const Tag = vocab.Tag;
const Envelope = vocab.Envelope;

/// Write one three-byte tag header at `at`, returning the offset of its value.
///
/// The length goes out low octet first, matching the byte order
/// `compose_tlv()` writes upstream.
fn writeTag(out: []u8, at: u16, tag_type: u8, len: u16) u16 {
    out[at + Tag.type_at] = tag_type;
    out[at + Tag.len_lo] = @truncate(len);
    out[at + Tag.len_hi] = @truncate(len >> 8);
    return at + Tag.value;
}

/// Read one tag's little-endian 16-bit length.
///
/// Reads what the sender wrote rather than assuming the host's own byte
/// order, which is what makes the parser portable to a big-endian host.
fn readLen(buf: []const u8, at: u16) u16 {
    const lo: u16 = buf[at + Tag.len_lo];
    const hi: u16 = buf[at + Tag.len_hi];
    return lo | (hi << 8);
}

/// Open an envelope in `out` for a protobuf body of `proto_len` bytes.
///
/// Returns the offset the body is to be written at, or null when `out` cannot
/// hold the body and the envelope around it.
pub fn open(out: []u8, proto_len: u16) ?u16 {
    const need: u32 = @as(u32, proto_len) + Envelope.overhead;
    if (out.len < need) {
        return null;
    }

    var at = writeTag(out, 0, Tag.epname, Ep.len);
    @memcpy(out[at..][0..Ep.len], Ep.rsp);
    at += Ep.len;
    return writeTag(out, at, Tag.data, proto_len);
}

/// True when `payload` names one of the two registered RPC endpoints.
///
/// Compares against both at once, so a response and an unsolicited event are
/// accepted by one pass. The loop is bounded by the shared name length and
/// does not exit early (NASA Rule 2).
fn named(payload: []const u8) bool {
    var ok = true;
    for (payload[Tag.value..][0..Ep.len], Ep.rsp, Ep.evt) |got, want_rsp, want_evt| {
        if ((got != want_rsp) and (got != want_evt)) {
            ok = false;
        }
    }
    return ok;
}

/// Return the protobuf body inside a received envelope, or null when the
/// payload is not a well-formed envelope addressed to this host.
pub fn body(payload: []const u8) ?[]const u8 {
    if (payload.len < Envelope.overhead) {
        return null;
    }
    if ((payload[Tag.type_at] != Tag.epname) or (payload[Envelope.data_tag] != Tag.data)) {
        return null;
    }
    if (readLen(payload, 0) != Ep.len) {
        return null;
    }
    if (!named(payload)) {
        return null;
    }

    const body_len = readLen(payload, Envelope.data_tag);
    const span: u32 = @as(u32, body_len) + Envelope.overhead;
    if ((body_len == 0) or (span > payload.len)) {
        return null;
    }
    return payload[Envelope.overhead..][0..body_len];
}
