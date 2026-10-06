//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the station credentials and the join: `ra8_c6link_sta_cfg_set`
//! and `ra8_c6link_wifi_join`, as `ra8_c6link_wifi.h` declares them. The
//! credentials go up in `Req_WifiSetConfig`, built with the vendored codec's
//! own initialisers, and a bare `Req_WifiConnect` follows through
//! `priv_c6link_bare_req`. Both reach `priv_c6link_rpc_call`, which stays in C
//! with the rest of the RPC layer for now.
//!
//! `WifiStaConfig` carries a scan threshold and a PMF configuration as nested
//! messages that protobuf allows to be absent. Upstream's own host always
//! sends both, so a co-processor that dereferences either without a null
//! check has never seen them missing; sending them costs a few bytes.

const std = @import("std");
const Err = @import("abi_err.zig");
const header = @import("c6link_rpc_c.zig");
const field_copy = @import("internal/field_copy.zig");
const sta_cfg = @import("internal/sta_cfg.zig");
const sta_policy = @import("internal/sta_policy.zig");

/// The private `ra8_c6link_internal.h` view, codec types included.
pub const c = header.c;

/// The ids the join is issued and answered under.
pub const Id = struct {
    pub const set_config: u32 = c.RPC_ID__Req_WifiSetConfig;
    pub const set_config_resp: u32 = c.RPC_ID__Resp_WifiSetConfig;
    pub const connect: u32 = c.RPC_ID__Req_WifiConnect;
};

/// Writable copies of the credentials for the codec's binary fields, which
/// are non-const because packing and unpacking share one type. Copying keeps
/// the caller's record `const` instead of casting it away. Every field is
/// sized to the protocol maximum, so a copy bounded by validated lengths fits.
pub const WireBuf = struct {
    ssid: [sta_cfg.Bound.ssid_max]u8 = @splat(0),
    pass: [sta_cfg.Bound.pass_max]u8 = @splat(0),
    bssid: [field_copy.Bound.mac_octets]u8 = @splat(0),

    /// Copy the declared octets of `cfg`; its lengths were validated already.
    pub fn stage(self: *WireBuf, cfg: *const c.ra8_c6link_sta_cfg_t) void {
        const ssid: [*]const u8 = @ptrCast(&cfg.ssid);
        const pass: [*]const u8 = @ptrCast(&cfg.pass);
        @memcpy(self.ssid[0..cfg.ssid_len], ssid[0..cfg.ssid_len]);
        @memcpy(self.pass[0..cfg.pass_len], pass[0..cfg.pass_len]);
        @memcpy(&self.bssid, cfg.bssid.octet[0..field_copy.Bound.mac_octets]);
    }

    /// Wipe the staged credentials once the request has been sent.
    pub fn wipe(self: *WireBuf) void {
        std.crypto.secureZero(u8, std.mem.asBytes(self));
    }
};

fn wipeCfg(cfg: *c.ra8_c6link_sta_cfg_t) void {
    std.crypto.secureZero(u8, std.mem.asBytes(cfg));
}

/// `ra8_c6link_sta_cfg_set`: fill a station record from two strings. The
/// record is wiped first, on every path, so a refusal never leaves a previous
/// passphrase behind. A null `pass` is the open network.
pub export fn ra8_c6link_sta_cfg_set(
    cfg: ?*c.ra8_c6link_sta_cfg_t,
    ssid: ?[*]const u8,
    pass: ?[*]const u8,
) callconv(.c) c.ra8_err_t {
    const dst = cfg orelse return Err.null_ptr;
    wipeCfg(dst);
    const name = ssid orelse return Err.null_ptr;

    const ssid_len = sta_cfg.length(name[0..dst.ssid.len]);
    const pass_len: u8 = if (pass) |p| sta_cfg.length(p[0..dst.pass.len]) else 0;
    sta_cfg.credentialsValid(ssid_len, pass_len) catch return Err.invalid_size;

    const ssid_out: [*]u8 = @ptrCast(&dst.ssid);
    const pass_out: [*]u8 = @ptrCast(&dst.pass);
    @memcpy(ssid_out[0..ssid_len], name[0..ssid_len]);
    if (pass) |p| @memcpy(pass_out[0..pass_len], p[0..pass_len]);
    dst.ssid_len = ssid_len;
    dst.pass_len = pass_len;
    return Err.ok;
}

/// The nested messages one `Req_WifiSetConfig` points into. They live in the
/// caller's frame, which outlives the call that packs them.
const Request = struct {
    threshold: c.WifiScanThreshold = undefined,
    pmf: c.WifiPmfConfig = undefined,
    sta: c.WifiStaConfig = undefined,
    wcfg: c.WifiConfig = undefined,
    body: c.RpcReqWifiSetConfig = undefined,
    rpc: c.Rpc = undefined,

    /// Point every message at the next and at the staged credentials.
    fn build(self: *Request, cfg: *const c.ra8_c6link_sta_cfg_t, buf: *WireBuf) void {
        const policy = sta_policy.policy();
        c.wifi_scan_threshold__init(&self.threshold);
        self.threshold.authmode = policy.auth_threshold;
        c.wifi_pmf_config__init(&self.pmf);
        self.pmf.capable = @intFromBool(policy.pmf_capable != 0);

        c.wifi_sta_config__init(&self.sta);
        self.sta.ssid = .{ .len = cfg.ssid_len, .data = &buf.ssid };
        self.sta.password = .{ .len = cfg.pass_len, .data = &buf.pass };
        self.sta.scan_method = policy.scan_method;
        self.sta.sort_method = policy.sort_method;
        self.sta.channel = cfg.channel;
        self.sta.bssid_set = @intFromBool(cfg.bssid_set);
        self.sta.bssid = .{ .len = sta_policy.bssidLen(cfg.bssid_set), .data = &buf.bssid };
        self.sta.threshold = &self.threshold;
        self.sta.pmf_cfg = &self.pmf;

        c.wifi_config__init(&self.wcfg);
        self.wcfg.u_case = c.WIFI_CONFIG__U_STA;
        self.wcfg.unnamed_0.sta = &self.sta;
        c.rpc__req__wifi_set_config__init(&self.body);
        self.body.iface = policy.iface;
        self.body.cfg = &self.wcfg;

        c.rpc__init(&self.rpc);
        self.rpc.msg_type = c.RPC_TYPE__Req;
        self.rpc.msg_id = c.RPC_ID__Req_WifiSetConfig;
        self.rpc.payload_case = c.RPC__PAYLOAD_REQ_WIFI_SET_CONFIG;
        self.rpc.unnamed_0.req_wifi_set_config = &self.body;
    }
};

/// Send the credentials and the search hints together: a known channel skips
/// a full scan and a known BSSID pins the association to one radio. The
/// strings go as counted binary fields, so an SSID with a zero octet, which
/// 802.11 permits, survives. The staged copy is wiped on every path.
fn setConfig(link: *c.ra8_c6link_t, cfg: *const c.ra8_c6link_sta_cfg_t) c.ra8_err_t {
    var buf = WireBuf{};
    buf.stage(cfg);
    defer buf.wipe();
    var req = Request{};
    req.build(cfg, &buf);
    var take = c.ra8_c6link_take_ctx_t{ .link = link, .out = null, .rpc_id = Id.set_config };
    return c.priv_c6link_rpc_call(link, &req.rpc, Id.set_config_resp, c.priv_c6link_take_resp, &take);
}

/// `ra8_c6link_wifi_join`: store the credentials, then ask to connect. The
/// connect is not sent when the configuration was refused, and the lengths
/// are checked again here because the record may have been filled by hand.
pub export fn ra8_c6link_wifi_join(link: ?*c.ra8_c6link_t, cfg: ?*const c.ra8_c6link_sta_cfg_t) callconv(.c) c.ra8_err_t {
    const handle = link orelse return Err.null_ptr;
    const record = cfg orelse return Err.null_ptr;
    if (!handle.open) return Err.not_initialized;
    sta_cfg.credentialsValid(record.ssid_len, record.pass_len) catch return Err.invalid_size;

    const configured = setConfig(handle, record);
    if (configured != Err.ok) return configured;
    return c.priv_c6link_bare_req(handle, Id.connect);
}
