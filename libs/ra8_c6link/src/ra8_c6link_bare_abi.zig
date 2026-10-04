//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for `priv_c6link_bare_req`: one of the five Wi-Fi requests whose body
//! is empty, built with the vendored codec's own initialiser and issued through
//! `priv_c6link_rpc_call` with the shared `priv_c6link_take_resp` extractor.
//! Which answer pairs with which request is `internal/bare_rpc.zig`'s rule.
//! The RPC layer and the extractor stay in C for now (RA8FW-640).

const Err = @import("abi_err.zig");
const header = @import("c6link_rpc_c.zig");
const bare_rpc = @import("internal/bare_rpc.zig");

pub const c = header.c;

/// Storage for whichever empty body goes out; only one is ever live.
const Body = extern union {
    start: c.RpcReqWifiStart,
    stop: c.RpcReqWifiStop,
    deinit: c.RpcReqWifiDeinit,
    connect: c.RpcReqWifiConnect,
    disconnect: c.RpcReqWifiDisconnect,
};

/// Point `req` at a freshly initialised body for `req_id`. False when
/// `req_id` is not one of the bare requests, leaving `req` without a payload.
fn attach(req: *c.Rpc, body: *Body, req_id: u32) bool {
    switch (req_id) {
        bare_rpc.Req.wifi_start => {
            c.rpc__req__wifi_start__init(&body.start);
            req.payload_case = c.RPC__PAYLOAD_REQ_WIFI_START;
            req.unnamed_0.req_wifi_start = &body.start;
        },
        bare_rpc.Req.wifi_stop => {
            c.rpc__req__wifi_stop__init(&body.stop);
            req.payload_case = c.RPC__PAYLOAD_REQ_WIFI_STOP;
            req.unnamed_0.req_wifi_stop = &body.stop;
        },
        bare_rpc.Req.wifi_deinit => {
            c.rpc__req__wifi_deinit__init(&body.deinit);
            req.payload_case = c.RPC__PAYLOAD_REQ_WIFI_DEINIT;
            req.unnamed_0.req_wifi_deinit = &body.deinit;
        },
        bare_rpc.Req.wifi_connect => {
            c.rpc__req__wifi_connect__init(&body.connect);
            req.payload_case = c.RPC__PAYLOAD_REQ_WIFI_CONNECT;
            req.unnamed_0.req_wifi_connect = &body.connect;
        },
        bare_rpc.Req.wifi_disconnect => {
            c.rpc__req__wifi_disconnect__init(&body.disconnect);
            req.payload_case = c.RPC__PAYLOAD_REQ_WIFI_DISCONNECT;
            req.unnamed_0.req_wifi_disconnect = &body.disconnect;
        },
        else => return false,
    }
    return true;
}

/// `priv_c6link_bare_req`: send the empty-body request `req_id` and return
/// the co-processor's verdict. A null link is `null_ptr`; an id that is not a
/// bare request is `not_supported`, and nothing is sent for either.
pub export fn priv_c6link_bare_req(link: ?*c.ra8_c6link_t, req_id: u32) callconv(.c) c.ra8_err_t {
    const handle = link orelse return Err.null_ptr;
    const resp_id = bare_rpc.respFor(req_id) orelse return Err.not_supported;

    var req: c.Rpc = undefined;
    c.rpc__init(&req);
    req.msg_type = c.RPC_TYPE__Req;
    req.msg_id = @intCast(req_id);

    var body: Body = undefined;
    if (!attach(&req, &body, req_id)) return Err.not_supported;

    var take = c.ra8_c6link_take_ctx_t{ .link = handle, .out = null, .rpc_id = req_id };
    return c.priv_c6link_rpc_call(handle, &req, resp_id, c.priv_c6link_take_resp, &take);
}
