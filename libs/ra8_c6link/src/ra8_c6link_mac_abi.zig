//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the station address query: `ra8_c6link_wifi_mac`, as
//! `ra8_c6link_wifi.h` declares it, and the response extractor it hands the
//! RPC layer. The request is built with the vendored codec's own initialisers
//! and issued through `priv_c6link_rpc_call`, which stays in C with the rest
//! of the RPC layer for now.

const std = @import("std");
const Err = @import("abi_err.zig");
const field = @import("ra8_c6link_field_abi.zig");
const header = @import("c6link_rpc_c.zig");
const sta_policy = @import("internal/sta_policy.zig");

/// The private `ra8_c6link_internal.h` view, codec types included.
pub const c = header.c;

/// The ids this query is issued and answered under.
pub const Id = struct {
    pub const request: u32 = c.RPC_ID__Req_GetMACAddress;
    pub const response: u32 = c.RPC_ID__Resp_GetMACAddress;
};

/// Response extractor. The co-processor's verdict is checked before the
/// address, so a refusal is reported as a refusal rather than as a malformed
/// address; an address of the wrong length is a protocol error.
fn takeMac(ctx: ?*anyopaque, msg_v: ?*const anyopaque) callconv(.c) c.ra8_err_t {
    const take: *c.ra8_c6link_take_ctx_t = @ptrCast(@alignCast(ctx.?));
    const msg: *const c.Rpc = @ptrCast(@alignCast(msg_v.?));
    const out: *c.ra8_c6link_mac_t = @ptrCast(@alignCast(take.out.?));

    const body: *const c.RpcRespGetMacAddress =
        msg.unnamed_0.resp_get_mac_address orelse return Err.protocol_error;
    const reported = c.priv_c6link_resp(take.link, take.rpc_id, body.resp);
    if (reported != Err.ok) return reported;
    return if (field.priv_c6link_copy_mac(@ptrCast(out), @ptrCast(&body.mac))) Err.ok else Err.protocol_error;
}

/// `ra8_c6link_wifi_mac`: read the station's own address. `out` is zeroed
/// before the request goes out, so a failed query never leaves a previous
/// answer behind. `Req_GetMACAddress.mode` is a `wifi_interface_t`: the
/// co-processor hands it to `esp_wifi_get_mac()`, so the station interface
/// from `internal/sta_policy.zig` is what returns the associating address.
pub export fn ra8_c6link_wifi_mac(link: ?*c.ra8_c6link_t, out: ?*c.ra8_c6link_mac_t) callconv(.c) c.ra8_err_t {
    const handle = link orelse return Err.null_ptr;
    const dst = out orelse return Err.null_ptr;
    if (!handle.open) return Err.not_initialized;
    dst.* = std.mem.zeroes(c.ra8_c6link_mac_t);

    var body: c.RpcReqGetMacAddress = undefined;
    c.rpc__req__get_mac_address__init(&body);
    body.mode = sta_policy.policy().iface;
    var req: c.Rpc = undefined;
    c.rpc__init(&req);
    req.msg_type = c.RPC_TYPE__Req;
    req.msg_id = c.RPC_ID__Req_GetMACAddress;
    req.payload_case = c.RPC__PAYLOAD_REQ_GET_MAC_ADDRESS;
    req.unnamed_0.req_get_mac_address = &body;

    var take = c.ra8_c6link_take_ctx_t{ .link = handle, .out = dst, .rpc_id = Id.request };
    return c.priv_c6link_rpc_call(handle, &req, Id.response, takeMac, &take);
}
