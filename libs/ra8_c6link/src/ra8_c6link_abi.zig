//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for the Zig half of `ra8_c6link`.
//!
//! The wire layers inside work in slices; this file is the only place raw
//! pointers, declared lengths, out-parameters and `ra8_err_t` codes are
//! handled, so the unchanged declarations in `src/ra8_c6link_internal.h` keep
//! working for the C translation units beside it and the `tests/wireless`
//! suites.

const caps = @import("internal/caps.zig");
const frame = @import("internal/frame.zig");
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

/// `ra8_c6link_frame_class_t`, which the C declares as an `enum : uint8_t`.
const Class = struct {
    pub const data: u8 = 0;
    pub const idle: u8 = 1;
    pub const malformed: u8 = 2;
    pub const bad_checksum: u8 = 3;
};

/// `ra8_c6link_rx_view_t`: where a classified frame's payload is.
const RxView = extern struct {
    offset: u16,
    len: u16,
    if_type: u8,
    if_num: u8,
};

/// `priv_c6link_frame_filler`: write the idle filler transaction.
pub export fn priv_c6link_frame_filler(tx: ?[*]u8) callconv(.c) void {
    const buf = tx orelse return;
    frame.filler(buf[0..frame.Frame.bytes]);
}

/// `priv_c6link_frame_seal`: header a transaction whose payload is staged.
pub export fn priv_c6link_frame_seal(
    tx: ?[*]u8,
    if_type: u8,
    if_num: u8,
    len: u16,
) callconv(.c) void {
    const buf = tx orelse return;
    _ = frame.seal(buf[0..frame.Frame.bytes], if_type, if_num, len);
}

/// `priv_c6link_frame_classify`: decide what a received transaction is.
///
/// Fills `view` only for a data frame, as the C did, so a caller that ignores
/// the verdict cannot read a payload the checksum never covered.
pub export fn priv_c6link_frame_classify(rx: ?[*]const u8, view: ?*RxView) callconv(.c) u8 {
    const buf = rx orelse return Class.malformed;
    const out = view orelse return Class.malformed;

    return switch (frame.classify(buf[0..frame.Frame.bytes])) {
        .idle => Class.idle,
        .malformed => Class.malformed,
        .bad_checksum => Class.bad_checksum,
        .data => |found| {
            out.* = .{
                .offset = found.offset,
                .len = found.len,
                .if_type = found.if_type,
                .if_num = found.if_num,
            };
            return Class.data;
        },
    };
}

/// `priv_c6link_caps`: build the host-capabilities announcement.
pub export fn priv_c6link_caps(out: ?[*]u8, cap: u8) callconv(.c) u8 {
    const buf = out orelse return 0;
    return caps.write(buf[0..cap]) orelse 0;
}
