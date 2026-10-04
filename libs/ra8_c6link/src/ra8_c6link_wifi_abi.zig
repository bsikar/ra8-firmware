//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the Wi-Fi lifecycle exports: `ra8_c6link_wifi_start`,
//! `ra8_c6link_wifi_stop` and `ra8_c6link_wifi_leave`, as `ra8_c6link.h`
//! declares them. Start builds `Req_WifiInit` and `Req_SetWifiMode` with the
//! vendored codec's own initialisers (RA8FW-638); everything else is a guard
//! plus bare requests. `priv_c6link_rpc_call`, `priv_c6link_take_resp` and
//! `priv_c6link_bare_req` stay in C with the rest of the RPC layer for now.

const Err = @import("abi_err.zig");
const header = @import("c6link_rpc_c.zig");
pub const wifi_init = @import("internal/wifi_init.zig");

/// The private `ra8_c6link_internal.h` view, codec ids included.
pub const c = header.c;

/// The requests these exports issue.
pub const Id = struct {
    pub const init: u32 = c.RPC_ID__Req_WifiInit;
    pub const init_resp: u32 = c.RPC_ID__Resp_WifiInit;
    pub const set_mode: u32 = c.RPC_ID__Req_SetWifiMode;
    pub const set_mode_resp: u32 = c.RPC_ID__Resp_SetWifiMode;
    pub const start: u32 = c.RPC_ID__Req_WifiStart;
    pub const stop: u32 = c.RPC_ID__Req_WifiStop;
    pub const deinit: u32 = c.RPC_ID__Req_WifiDeinit;
    pub const disconnect: u32 = c.RPC_ID__Req_WifiDisconnect;
};

/// Station in the co-processor's own `wifi_mode_t` numbering (ESP-IDF:
/// 0 null, 1 station, 2 access point), which is what crosses the link.
pub const mode_sta: i32 = 1;

/// The open link behind `link`, or the code a closed or null one returns.
fn opened(link: ?*c.ra8_c6link_t) error{ Null, Closed }!*c.ra8_c6link_t {
    const handle = link orelse return error.Null;
    if (!handle.open) return error.Closed;
    return handle;
}

fn guardCode(err: error{ Null, Closed }) c.ra8_err_t {
    return switch (err) {
        error.Null => Err.null_ptr,
        error.Closed => Err.not_initialized,
    };
}

/// `ra8_c6link_wifi_stop`: stop the station, then deinit the Wi-Fi driver.
/// Deinit is sent even when stop fails; stop's error wins when both fail.
pub export fn ra8_c6link_wifi_stop(link: ?*c.ra8_c6link_t) callconv(.c) c.ra8_err_t {
    const handle = opened(link) catch |err| return guardCode(err);
    const stopped = c.priv_c6link_bare_req(handle, Id.stop);
    const deinit = c.priv_c6link_bare_req(handle, Id.deinit);
    return if (stopped != Err.ok) stopped else deinit;
}

/// `ra8_c6link_wifi_leave`: disconnect from the current access point.
pub export fn ra8_c6link_wifi_leave(link: ?*c.ra8_c6link_t) callconv(.c) c.ra8_err_t {
    const handle = opened(link) catch |err| return guardCode(err);
    return c.priv_c6link_bare_req(handle, Id.disconnect);
}

/// Issue `req` and let the shared extractor record the verdict under `req_id`.
fn call(handle: *c.ra8_c6link_t, req: *c.Rpc, req_id: u32, resp_id: u32) c.ra8_err_t {
    var take = c.ra8_c6link_take_ctx_t{ .link = handle, .out = null, .rpc_id = req_id };
    return c.priv_c6link_rpc_call(handle, req, resp_id, c.priv_c6link_take_resp, &take);
}

/// `Req_WifiInit` carrying the configuration from `internal/wifi_init.zig`,
/// where every value is documented. The co-processor validates `magic`.
fn doInit(handle: *c.ra8_c6link_t) c.ra8_err_t {
    const set = wifi_init.cfg();
    var cfg: c.WifiInitConfig = undefined;
    c.wifi_init_config__init(&cfg);
    cfg.static_rx_buf_num = set.static_rx_buf_num;
    cfg.dynamic_rx_buf_num = set.dynamic_rx_buf_num;
    cfg.tx_buf_type = set.tx_buf_type;
    cfg.static_tx_buf_num = set.static_tx_buf_num;
    cfg.dynamic_tx_buf_num = set.dynamic_tx_buf_num;
    cfg.rx_mgmt_buf_type = set.rx_mgmt_buf_type;
    cfg.rx_mgmt_buf_num = set.rx_mgmt_buf_num;
    cfg.ampdu_rx_enable = set.ampdu_rx_enable;
    cfg.ampdu_tx_enable = set.ampdu_tx_enable;
    cfg.nvs_enable = set.nvs_enable;
    cfg.rx_ba_win = set.rx_ba_win;
    cfg.beacon_max_len = set.beacon_max_len;
    cfg.mgmt_sbuf_num = set.mgmt_sbuf_num;
    cfg.feature_caps = set.feature_caps;
    cfg.sta_disconnected_pm = @intFromBool(set.sta_disconnected_pm != 0);
    cfg.espnow_max_encrypt_num = set.espnow_max_encrypt_num;
    cfg.tx_hetb_queue_num = set.tx_hetb_queue_num;
    cfg.magic = set.magic;

    var body: c.RpcReqWifiInit = undefined;
    c.rpc__req__wifi_init__init(&body);
    body.cfg = &cfg;
    var req: c.Rpc = undefined;
    c.rpc__init(&req);
    req.msg_type = c.RPC_TYPE__Req;
    req.msg_id = c.RPC_ID__Req_WifiInit;
    req.payload_case = c.RPC__PAYLOAD_REQ_WIFI_INIT;
    req.unnamed_0.req_wifi_init = &body;
    return call(handle, &req, Id.init, Id.init_resp);
}

/// `Req_SetWifiMode` selecting station mode, before any credential is sent,
/// which is the order ESP-IDF's own station bring-up uses.
fn doMode(handle: *c.ra8_c6link_t) c.ra8_err_t {
    var body: c.RpcReqSetMode = undefined;
    c.rpc__req__set_mode__init(&body);
    body.mode = mode_sta;
    var req: c.Rpc = undefined;
    c.rpc__init(&req);
    req.msg_type = c.RPC_TYPE__Req;
    req.msg_id = c.RPC_ID__Req_SetWifiMode;
    req.payload_case = c.RPC__PAYLOAD_REQ_SET_WIFI_MODE;
    req.unnamed_0.req_set_wifi_mode = &body;
    return call(handle, &req, Id.set_mode, Id.set_mode_resp);
}

/// `ra8_c6link_wifi_start`: initialise the co-processor's Wi-Fi stack, put it
/// in station mode, then start it. The first failure is returned and nothing
/// after it is sent.
pub export fn ra8_c6link_wifi_start(link: ?*c.ra8_c6link_t) callconv(.c) c.ra8_err_t {
    const handle = opened(link) catch |err| return guardCode(err);
    const inited = doInit(handle);
    if (inited != Err.ok) return inited;
    const moded = doMode(handle);
    if (moded != Err.ok) return moded;
    return c.priv_c6link_bare_req(handle, Id.start);
}
