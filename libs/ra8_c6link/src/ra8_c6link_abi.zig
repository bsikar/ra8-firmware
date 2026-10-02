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

const std = @import("std");

const caps = @import("internal/caps.zig");
const frame = @import("internal/frame.zig");
const rpc_wait = @import("internal/rpc_wait.zig");
const sta_cfg = @import("internal/sta_cfg.zig");
const rx_route = @import("internal/rx_route.zig");
const field_copy = @import("internal/field_copy.zig");
const tx_admit = @import("internal/tx_admit.zig");
const wifi_init = @import("internal/wifi_init.zig");
const bare_rpc = @import("internal/bare_rpc.zig");
const sta_policy = @import("internal/sta_policy.zig");
const tlv = @import("internal/tlv.zig");

const Err = @import("abi_err.zig");

comptime {
    _ = @import("ra8_c6link_arena_abi.zig");
}

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

/// `priv_c6link_rpc_issuable`: may another request go out on this link?
///
/// Returns `k_ra8_ok`, `k_ra8_err_not_initialized`, or `k_ra8_err_busy`, so
/// the caller returns the verdict as it stands.
pub export fn priv_c6link_rpc_issuable(open: bool, armed: bool, tx_len: u16) callconv(.c) u16 {
    rpc_wait.issuable(open, .{ .uid = 0, .resp_id = 0, .armed = armed }, tx_len) catch |e| {
        return switch (e) {
            error.NotInitialized => Err.not_initialized,
            error.Busy => Err.busy,
        };
    };
    return Err.ok;
}

/// `priv_c6link_rpc_answers`: is this decoded response the outstanding answer?
pub export fn priv_c6link_rpc_answers(
    armed: bool,
    wait_uid: u32,
    wait_resp_id: u32,
    msg_uid: u32,
    msg_id: u32,
) callconv(.c) bool {
    return rpc_wait.answers(
        .{ .uid = wait_uid, .resp_id = wait_resp_id, .armed = armed },
        .{ .uid = msg_uid, .msg_id = msg_id },
    );
}

/// `priv_c6link_sta_len`: measure a credential that may not be terminated.
///
/// Returns `cap` when no terminator was found inside the buffer, which the
/// caller refuses on length rather than reading further.
pub export fn priv_c6link_sta_len(text: ?[*]const u8, cap: u8) callconv(.c) u8 {
    const buf = text orelse return 0;
    return sta_cfg.length(buf[0..cap]);
}

/// `priv_c6link_sta_credentials_valid`: are these one joinable network's lengths?
pub export fn priv_c6link_sta_credentials_valid(ssid_len: u8, pass_len: u8) callconv(.c) bool {
    sta_cfg.credentialsValid(ssid_len, pass_len) catch return false;
    return true;
}

/// `priv_c6link_rx_route`: which consumer this frame's interface number belongs to.
///
/// Returns the `rx_route.Route` ordinal, which `priv_c6link_route_t` mirrors.
pub export fn priv_c6link_rx_route(if_type: u8) callconv(.c) u8 {
    return @intFromEnum(rx_route.routeFor(if_type));
}

/// `priv_c6link_field_take`: octets of a text field that fit a `cap`-octet destination.
pub export fn priv_c6link_field_take(src_len: usize, cap: u8) callconv(.c) usize {
    return field_copy.strTake(src_len, cap);
}

/// `priv_c6link_field_is_mac`: does this field carry exactly one hardware address?
pub export fn priv_c6link_field_is_mac(src_len: usize) callconv(.c) bool {
    return field_copy.macAcceptable(src_len);
}

/// `priv_c6link_tx_admit`: may this Ethernet frame go out right now?
///
/// Returns `k_ra8_ok`, `k_ra8_err_not_initialized`, `k_ra8_err_invalid_size`
/// or `k_ra8_err_busy`, so the caller returns the verdict as it stands.
pub export fn priv_c6link_tx_admit(open: bool, len: u16, tx_len: u16) callconv(.c) u16 {
    tx_admit.admit(open, len, tx_len) catch |e| {
        return switch (e) {
            error.NotInitialized => Err.not_initialized,
            error.InvalidSize => Err.invalid_size,
            error.Busy => Err.busy,
        };
    };
    return Err.ok;
}

/// `priv_c6link_wifi_init_cfg`: the configuration `Req_WifiInit` carries.
///
/// Writes the validated set into @p out, which the caller copies field by
/// field into the generated `WifiInitConfig`.
pub export fn priv_c6link_wifi_init_cfg(out: ?*wifi_init.Cfg) callconv(.c) void {
    const dst = out orelse return;
    dst.* = wifi_init.cfg();
}

/// `priv_c6link_bare_resp`: which answer id pairs with this bare request?
///
/// Writes the `RPC_ID__Resp_*` that answers @p req_id into @p out and returns
/// true. Returns false, leaving @p out at zero, when @p req_id is not one of
/// the requests whose body is empty.
pub export fn priv_c6link_bare_resp(req_id: u32, out: ?*u32) callconv(.c) bool {
    const dst = out orelse return false;
    dst.* = 0;
    const resp = bare_rpc.respFor(req_id) orelse return false;
    dst.* = resp;
    return true;
}

/// `priv_c6link_sta_policy`: the selectors one station join transmits.
///
/// Writes the interface index, scan method, sort order, auth threshold and
/// PMF capability into @p out, which the caller copies into the generated
/// `WifiStaConfig`.
pub export fn priv_c6link_sta_policy(out: ?*sta_policy.Policy) callconv(.c) void {
    const dst = out orelse return;
    dst.* = sta_policy.policy();
}

/// `priv_c6link_sta_bssid_len`: octets of BSSID this join puts on the wire.
pub export fn priv_c6link_sta_bssid_len(pinned: bool) callconv(.c) usize {
    return sta_policy.bssidLen(pinned);
}
