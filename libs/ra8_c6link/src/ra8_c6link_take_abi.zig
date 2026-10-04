//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for `priv_c6link_take_resp`: the response extractor every Wi-Fi
//! request hands `priv_c6link_rpc_call`. It reads the co-processor's `resp`
//! from the body that matches the answer id and records it through
//! `priv_c6link_resp` under the request id. The RPC layer stays in C for now
//! (RA8FW-642).

const header = @import("c6link_rpc_c.zig");

pub const c = header.c;

/// The verdict reported when the answer carries no body or is not a Wi-Fi
/// answer at all: never a code the co-processor sends for success.
const no_verdict: i32 = -1;

/// `resp` out of the body field `name`, or `no_verdict` when it is absent.
fn verdict(msg: *const c.Rpc, comptime name: []const u8) i32 {
    const body = @field(msg.unnamed_0, name) orelse return no_verdict;
    return body.*.resp;
}

/// The co-processor's verdict for one Wi-Fi answer. Every arm names its body
/// outright, so a renumbered answer cannot be read through the wrong field.
fn verdictFor(msg: *const c.Rpc) i32 {
    return switch (msg.msg_id) {
        c.RPC_ID__Resp_WifiInit => verdict(msg, "resp_wifi_init"),
        c.RPC_ID__Resp_SetWifiMode => verdict(msg, "resp_set_wifi_mode"),
        c.RPC_ID__Resp_WifiSetConfig => verdict(msg, "resp_wifi_set_config"),
        c.RPC_ID__Resp_WifiStart => verdict(msg, "resp_wifi_start"),
        c.RPC_ID__Resp_WifiStop => verdict(msg, "resp_wifi_stop"),
        c.RPC_ID__Resp_WifiDeinit => verdict(msg, "resp_wifi_deinit"),
        c.RPC_ID__Resp_WifiConnect => verdict(msg, "resp_wifi_connect"),
        c.RPC_ID__Resp_WifiDisconnect => verdict(msg, "resp_wifi_disconnect"),
        else => no_verdict,
    };
}

/// `priv_c6link_take_resp`: record the verdict of a Wi-Fi answer. `ctx` is a
/// `ra8_c6link_take_ctx_t` naming the link and the request id; `msg_v` is the
/// decoded `Rpc` the RPC layer matched against the expected answer id.
pub export fn priv_c6link_take_resp(ctx: ?*anyopaque, msg_v: ?*const anyopaque) callconv(.c) c.ra8_err_t {
    const take: *c.ra8_c6link_take_ctx_t = @ptrCast(@alignCast(ctx.?));
    const msg: *const c.Rpc = @ptrCast(@alignCast(msg_v.?));
    return c.priv_c6link_resp(take.link, take.rpc_id, verdictFor(msg));
}
