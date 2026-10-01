//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for the Zig half of `ra8_c6link`.
//!
//! The envelope codec inside works in slices; this file is the only place
//! raw pointers, declared lengths and `ra8_err_t` codes are handled, so the
//! unchanged declarations in `src/ra8_c6link_internal.h` keep working for
//! `ra8_c6link_rpc.c` and the `tests/wireless` suites.

const tlv = @import("internal/tlv.zig");

/// Subset of `ra8_err_t` this library returns.
const Err = struct {
    pub const ok: u16 = 0;
    pub const invalid_size: u16 = 0x105;
    pub const null_ptr: u16 = 0x504;
};

/// `priv_c6link_tlv_open`: open an envelope for a `proto_len`-byte body.
///
/// Writes both tag headers and the endpoint name into `out`, then reports
/// through `body_at` the offset the protobuf body is to be written at.
pub export fn priv_c6link_tlv_open(
    out: ?[*]u8,
    cap: u16,
    proto_len: u16,
    body_at: ?*u16,
) callconv(.c) u16 {
    const at = body_at orelse return Err.null_ptr;
    const buf = out orelse return Err.null_ptr;
    at.* = 0;

    const offset = tlv.open(buf[0..cap], proto_len) orelse return Err.invalid_size;
    at.* = offset;
    return Err.ok;
}

/// `priv_c6link_tlv_body`: find the protobuf body inside a received envelope.
///
/// Returns null and leaves `proto_len` zero when the payload is not a
/// well-formed envelope addressed to one of the two RPC endpoints.
pub export fn priv_c6link_tlv_body(
    payload: ?[*]const u8,
    len: u16,
    proto_len: ?*u16,
) callconv(.c) ?[*]const u8 {
    const out_len = proto_len orelse return null;
    const buf = payload orelse return null;
    out_len.* = 0;

    const found = tlv.body(buf[0..len]) orelse return null;
    out_len.* = @intCast(found.len);
    return found.ptr;
}
