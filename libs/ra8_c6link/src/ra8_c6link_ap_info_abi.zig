//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the associated-AP query: `ra8_c6link_wifi_ap_info`, as
//! `ra8_c6link_wifi.h` declares it, and the response extractor it hands the
//! RPC layer. The request is built with the vendored codec's own initialisers
//! and issued through `priv_c6link_rpc_call`, which stays in C with the rest
//! of the RPC layer for now.

const std = @import("std");
const Err = @import("abi_err.zig");
const field = @import("ra8_c6link_field_abi.zig");
const header = @import("c6link_rpc_c.zig");

/// The private `ra8_c6link_internal.h` view, codec types included.
pub const c = header.c;

/// The ids this query is issued and answered under.
pub const Id = struct {
    pub const request: u32 = c.RPC_ID__Req_WifiStaGetApInfo;
    pub const response: u32 = c.RPC_ID__Resp_WifiStaGetApInfo;
};

/// Response extractor. An unassociated station is answered with a failure
/// code rather than an empty record, so the verdict is checked before the
/// record is read; on failure the caller's record stays cleared.
fn takeAp(ctx: ?*anyopaque, msg_v: ?*const anyopaque) callconv(.c) c.ra8_err_t {
    const take: *c.ra8_c6link_take_ctx_t = @ptrCast(@alignCast(ctx.?));
    const msg: *const c.Rpc = @ptrCast(@alignCast(msg_v.?));
    const out: *c.ra8_c6link_ap_info_t = @ptrCast(@alignCast(take.out.?));

    const body: *const c.RpcRespWifiStaGetApInfo =
        msg.unnamed_0.resp_wifi_sta_get_ap_info orelse return Err.protocol_error;
    const reported = c.priv_c6link_resp(take.link, take.rpc_id, body.resp);
    if (reported != Err.ok) return reported;
    const rec: *const c.WifiApRecord = body.ap_record orelse return Err.protocol_error;

    out.ssid_len = field.priv_c6link_copy_str(&out.ssid, out.ssid.len, @ptrCast(&rec.ssid));
    out.channel = @truncate(@as(u32, @bitCast(rec.primary)));
    out.rssi = @truncate(rec.rssi);
    out.authmode = @intCast(rec.authmode);
    _ = field.priv_c6link_copy_mac(@ptrCast(&out.bssid), @ptrCast(&rec.bssid));
    return Err.ok;
}

/// `ra8_c6link_wifi_ap_info`: read back the access point the station is
/// associated with. `out` is zeroed before the request goes out, so a failed
/// query never leaves a previous answer behind.
pub export fn ra8_c6link_wifi_ap_info(link: ?*c.ra8_c6link_t, out: ?*c.ra8_c6link_ap_info_t) callconv(.c) c.ra8_err_t {
    const handle = link orelse return Err.null_ptr;
    const dst = out orelse return Err.null_ptr;
    if (!handle.open) return Err.not_initialized;
    dst.* = std.mem.zeroes(c.ra8_c6link_ap_info_t);

    var body: c.RpcReqWifiStaGetApInfo = undefined;
    c.rpc__req__wifi_sta_get_ap_info__init(&body);
    var req: c.Rpc = undefined;
    c.rpc__init(&req);
    req.msg_type = c.RPC_TYPE__Req;
    req.msg_id = c.RPC_ID__Req_WifiStaGetApInfo;
    req.payload_case = c.RPC__PAYLOAD_REQ_WIFI_STA_GET_AP_INFO;
    req.unnamed_0.req_wifi_sta_get_ap_info = &body;

    var take = c.ra8_c6link_take_ctx_t{ .link = handle, .out = dst, .rpc_id = Id.request };
    return c.priv_c6link_rpc_call(handle, &req, Id.response, takeAp, &take);
}
