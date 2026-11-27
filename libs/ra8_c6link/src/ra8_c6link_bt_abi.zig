//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for Bluetooth controller lifecycle exports over esp-hosted
//! `Req_FeatureControl`: `ra8_c6link_bt_start` and `ra8_c6link_bt_stop`,
//! as `ra8_c6link.h` declares them.

const Err = @import("abi_err.zig");
const header = @import("c6link_rpc_c.zig");

/// The private `ra8_c6link_internal.h` view, codec ids included.
pub const c = header.c;

/// The request and answer ids paired for Bluetooth FeatureControl RPCs.
pub const Id = struct {
    pub const req: u32 = c.RPC_ID__Req_FeatureControl;
    pub const resp: u32 = c.RPC_ID__Resp_FeatureControl;
};

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

fn isTransportFault(err: c.ra8_err_t) bool {
    return err == Err.hw_timeout or err == Err.spi_error or err == Err.timeout or err == Err.busy;
}

fn featureControl(handle: *c.ra8_c6link_t, command: c.RpcFeatureCommand) c.ra8_err_t {
    var body: c.RpcReqFeatureControl = undefined;
    c.rpc__req__feature_control__init(&body);
    body.feature = c.RPC_FEATURE__Feature_Bluetooth;
    body.command = command;
    body.option = c.RPC_FEATURE_OPTION__Feature_Option_None;

    var req: c.Rpc = undefined;
    c.rpc__init(&req);
    req.msg_type = c.RPC_TYPE__Req;
    req.msg_id = c.RPC_ID__Req_FeatureControl;
    req.payload_case = c.RPC__PAYLOAD_REQ_FEATURE_CONTROL;
    req.unnamed_0.req_feature_control = &body;

    var take = c.ra8_c6link_take_ctx_t{ .link = handle, .out = null, .rpc_id = Id.req };
    return c.priv_c6link_rpc_call(handle, &req, Id.resp, c.priv_c6link_take_resp, &take);
}

fn resetStaleController(handle: *c.ra8_c6link_t) c.ra8_err_t {
    const disabled = featureControl(handle, c.RPC_FEATURE_COMMAND__Feature_Command_BT_Disable);
    if (isTransportFault(disabled)) return disabled;
    return featureControl(handle, c.RPC_FEATURE_COMMAND__Feature_Command_BT_Deinit);
}

/// `ra8_c6link_bt_stop`: disable and deinitialise the co-processor's Bluetooth
/// controller. Deinit still runs when disable fails; disable's error wins when
/// both fail.
pub export fn ra8_c6link_bt_stop(link: ?*c.ra8_c6link_t) callconv(.c) c.ra8_err_t {
    const handle = opened(link) catch |err| return guardCode(err);
    const disabled = featureControl(handle, c.RPC_FEATURE_COMMAND__Feature_Command_BT_Disable);
    const deinited = featureControl(handle, c.RPC_FEATURE_COMMAND__Feature_Command_BT_Deinit);
    return if (disabled != Err.ok) disabled else deinited;
}

/// `ra8_c6link_bt_start`: initialise and enable the co-processor's Bluetooth
/// controller over `Req_FeatureControl`. When the co-processor stayed powered
/// across a host reset and reports `invalid_state` on init, resets the stale
/// controller state (`BT_Disable` + `BT_Deinit`) and retries init once.
pub export fn ra8_c6link_bt_start(link: ?*c.ra8_c6link_t) callconv(.c) c.ra8_err_t {
    const handle = opened(link) catch |err| return guardCode(err);
    var inited = featureControl(handle, c.RPC_FEATURE_COMMAND__Feature_Command_BT_Init);
    if (inited == Err.invalid_state) {
        const reset = resetStaleController(handle);
        if (reset != Err.ok) return reset;
        inited = featureControl(handle, c.RPC_FEATURE_COMMAND__Feature_Command_BT_Init);
    }
    if (inited != Err.ok) return inited;
    return featureControl(handle, c.RPC_FEATURE_COMMAND__Feature_Command_BT_Enable);
}
