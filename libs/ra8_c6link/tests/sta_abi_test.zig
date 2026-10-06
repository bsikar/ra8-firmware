//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! `ra8_c6link_sta_cfg_set` and `ra8_c6link_wifi_join` against a scripted RPC
//! layer: the record they fill, the request they build, the wipe of the
//! staged credentials, the connect that follows, and the guards.

const std = @import("std");
const sta = @import("sta_abi");

const c = sta.c;

const Code = struct {
    pub const ok: u16 = 0;
    pub const invalid_size: u16 = 0x105;
    pub const not_initialized: u16 = 0x10F;
    pub const null_ptr: u16 = 0x504;
    pub const protocol_error: u16 = 0x406;
};

const Seen = struct {
    msg_id: u32 = 0,
    payload: u32 = 0,
    resp_id: u32 = 0,
    iface: i32 = -1,
    u_case: u32 = 0,
    ssid: [32]u8 = @splat(0),
    ssid_len: usize = 0,
    pass_len: usize = 0,
    bssid_set: i32 = -1,
    bssid_len: usize = 99,
    channel: u32 = 0,
    authmode: i32 = -1,
    pmf_capable: i32 = -1,
    take_rpc_id: u32 = 0,
    ssid_ptr: ?[*]const u8 = null,
};

const Script = struct {
    calls: usize = 0,
    bare: [4]u32 = @splat(0),
    bare_calls: usize = 0,
    config_verdict: u16 = 0,
    bare_verdict: u16 = 0,
    seen: Seen = .{},
};

var script: Script = .{};

export fn rpc__init(m: ?*c.Rpc) callconv(.c) void {
    m.?.* = std.mem.zeroes(c.Rpc);
}
export fn rpc__req__wifi_set_config__init(m: ?*c.RpcReqWifiSetConfig) callconv(.c) void {
    m.?.* = std.mem.zeroes(c.RpcReqWifiSetConfig);
}
export fn wifi_config__init(m: ?*c.WifiConfig) callconv(.c) void {
    m.?.* = std.mem.zeroes(c.WifiConfig);
}
export fn wifi_sta_config__init(m: ?*c.WifiStaConfig) callconv(.c) void {
    m.?.* = std.mem.zeroes(c.WifiStaConfig);
}
export fn wifi_scan_threshold__init(m: ?*c.WifiScanThreshold) callconv(.c) void {
    m.?.* = std.mem.zeroes(c.WifiScanThreshold);
    m.?.authmode = 77;
}
export fn wifi_pmf_config__init(m: ?*c.WifiPmfConfig) callconv(.c) void {
    m.?.* = std.mem.zeroes(c.WifiPmfConfig);
}
export fn priv_c6link_take_resp(ctx: ?*anyopaque, msg: ?*const anyopaque) callconv(.c) c.ra8_err_t {
    _ = msg;
    const take: *c.ra8_c6link_take_ctx_t = @ptrCast(@alignCast(ctx.?));
    script.seen.take_rpc_id = take.rpc_id;
    return script.config_verdict;
}
export fn priv_c6link_bare_req(link: ?*c.ra8_c6link_t, req_id: u32) callconv(.c) c.ra8_err_t {
    _ = link;
    script.bare[script.bare_calls] = req_id;
    script.bare_calls += 1;
    return script.bare_verdict;
}

fn record(req: *const c.Rpc, resp_id: u32) void {
    const s = &script.seen;
    s.msg_id = @intCast(req.msg_id);
    s.payload = @intCast(req.payload_case);
    s.resp_id = resp_id;
    const body = req.unnamed_0.req_wifi_set_config.?;
    s.iface = body.*.iface;
    const wcfg = body.*.cfg.?;
    s.u_case = @intCast(wcfg.*.u_case);
    const st = wcfg.*.unnamed_0.sta.?;
    s.ssid_len = st.*.ssid.len;
    s.ssid_ptr = st.*.ssid.data;
    @memcpy(s.ssid[0..s.ssid_len], st.*.ssid.data[0..s.ssid_len]);
    s.pass_len = st.*.password.len;
    s.bssid_set = st.*.bssid_set;
    s.bssid_len = st.*.bssid.len;
    s.channel = st.*.channel;
    s.authmode = st.*.threshold.?.*.authmode;
    s.pmf_capable = st.*.pmf_cfg.?.*.capable;
}

export fn priv_c6link_rpc_call(link: ?*c.ra8_c6link_t, req: ?*c.Rpc, resp_id: u32, take: c.ra8_c6link_take_fn_t, ctx: ?*anyopaque) callconv(.c) c.ra8_err_t {
    _ = link;
    script.calls += 1;
    record(req.?, resp_id);
    var resp = std.mem.zeroes(c.Rpc);
    return take.?(ctx, &resp);
}

fn openLink() c.ra8_c6link_t {
    var link = std.mem.zeroes(c.ra8_c6link_t);
    link.open = true;
    return link;
}

fn filled(ssid: [*:0]const u8, pass: ?[*:0]const u8) !c.ra8_c6link_sta_cfg_t {
    var cfg = std.mem.zeroes(c.ra8_c6link_sta_cfg_t);
    try std.testing.expectEqual(Code.ok, sta.ra8_c6link_sta_cfg_set(&cfg, ssid, pass));
    return cfg;
}

test "sta_cfg_set copies both strings and their lengths" {
    const cfg = try filled("ra8-bench", "hunter22");
    try std.testing.expectEqual(@as(u8, 9), cfg.ssid_len);
    try std.testing.expectEqual(@as(u8, 8), cfg.pass_len);
    try std.testing.expectEqualStrings("ra8-bench", std.mem.sliceTo(@as([*:0]const u8, @ptrCast(&cfg.ssid)), 0));
    try std.testing.expectEqualStrings("hunter22", std.mem.sliceTo(@as([*:0]const u8, @ptrCast(&cfg.pass)), 0));
}

test "sta_cfg_set takes a null passphrase as the open network" {
    const cfg = try filled("open-net", null);
    try std.testing.expectEqual(@as(u8, 0), cfg.pass_len);
}

test "sta_cfg_set wipes the record on every refusal" {
    var cfg = try filled("old-net", "old-secret");
    try std.testing.expectEqual(Code.null_ptr, sta.ra8_c6link_sta_cfg_set(&cfg, null, "x"));
    try std.testing.expectEqual(@as(u8, 0), cfg.pass[0]);
    cfg = try filled("old-net", "old-secret");
    try std.testing.expectEqual(Code.invalid_size, sta.ra8_c6link_sta_cfg_set(&cfg, "", "new"));
    try std.testing.expectEqual(@as(u8, 0), cfg.pass[0]);
    try std.testing.expectEqual(Code.null_ptr, sta.ra8_c6link_sta_cfg_set(null, "a", "b"));
}

test "sta_cfg_set refuses an SSID over 32 octets and a passphrase over 64" {
    var cfg = std.mem.zeroes(c.ra8_c6link_sta_cfg_t);
    const long_ssid: [33:0]u8 = @splat('a');
    const ok_ssid: [32:0]u8 = @splat('a');
    const long_pass: [65:0]u8 = @splat('p');
    const ok_pass: [64:0]u8 = @splat('p');
    try std.testing.expectEqual(Code.invalid_size, sta.ra8_c6link_sta_cfg_set(&cfg, &long_ssid, null));
    try std.testing.expectEqual(Code.invalid_size, sta.ra8_c6link_sta_cfg_set(&cfg, "ok", &long_pass));
    try std.testing.expectEqual(Code.ok, sta.ra8_c6link_sta_cfg_set(&cfg, &ok_ssid, &ok_pass));
}

test "wifi_join guards a null or closed link and a bad record" {
    script = .{};
    var link = openLink();
    var cfg = try filled("net", "pw");
    try std.testing.expectEqual(Code.null_ptr, sta.ra8_c6link_wifi_join(null, &cfg));
    try std.testing.expectEqual(Code.null_ptr, sta.ra8_c6link_wifi_join(&link, null));
    var closed = std.mem.zeroes(c.ra8_c6link_t);
    try std.testing.expectEqual(Code.not_initialized, sta.ra8_c6link_wifi_join(&closed, &cfg));
    cfg.ssid_len = 0;
    try std.testing.expectEqual(Code.invalid_size, sta.ra8_c6link_wifi_join(&link, &cfg));
    try std.testing.expectEqual(@as(usize, 0), script.calls + script.bare_calls);
}

test "wifi_join sends the station config, then connect" {
    script = .{};
    var link = openLink();
    var cfg = try filled("ra8-bench", "hunter22");
    cfg.channel = 6;
    try std.testing.expectEqual(Code.ok, sta.ra8_c6link_wifi_join(&link, &cfg));
    const s = script.seen;
    try std.testing.expectEqual(sta.Id.set_config, s.msg_id);
    try std.testing.expectEqual(@as(u32, c.RPC__PAYLOAD_REQ_WIFI_SET_CONFIG), s.payload);
    try std.testing.expectEqual(sta.Id.set_config_resp, s.resp_id);
    try std.testing.expectEqual(sta.Id.set_config, s.take_rpc_id);
    try std.testing.expectEqual(@as(u32, c.WIFI_CONFIG__U_STA), s.u_case);
    try std.testing.expectEqual(@as(i32, 0), s.iface);
    try std.testing.expectEqualStrings("ra8-bench", s.ssid[0..s.ssid_len]);
    try std.testing.expectEqual(@as(usize, 8), s.pass_len);
    try std.testing.expectEqual(@as(u32, 6), s.channel);
    try std.testing.expectEqual(@as(i32, 0), s.authmode);
    try std.testing.expectEqual(@as(i32, 1), s.pmf_capable);
    try std.testing.expectEqual(@as(i32, 0), s.bssid_set);
    try std.testing.expectEqual(@as(usize, 0), s.bssid_len);
    try std.testing.expectEqual(@as(usize, 1), script.bare_calls);
    try std.testing.expectEqual(sta.Id.connect, script.bare[0]);
}

test "wifi_join sends a pinned BSSID at full length" {
    script = .{};
    var link = openLink();
    var cfg = try filled("net", null);
    cfg.bssid_set = true;
    cfg.bssid.octet = .{ 1, 2, 3, 4, 5, 6 };
    try std.testing.expectEqual(Code.ok, sta.ra8_c6link_wifi_join(&link, &cfg));
    try std.testing.expectEqual(@as(i32, 1), script.seen.bssid_set);
    try std.testing.expectEqual(@as(usize, 6), script.seen.bssid_len);
}

test "wifi_join skips connect when the config is refused" {
    script = .{ .config_verdict = Code.protocol_error };
    var link = openLink();
    var cfg = try filled("net", "pw");
    try std.testing.expectEqual(Code.protocol_error, sta.ra8_c6link_wifi_join(&link, &cfg));
    try std.testing.expectEqual(@as(usize, 0), script.bare_calls);
}

test "wifi_join returns the connect verdict" {
    script = .{ .bare_verdict = Code.not_initialized };
    var link = openLink();
    var cfg = try filled("net", "pw");
    try std.testing.expectEqual(Code.not_initialized, sta.ra8_c6link_wifi_join(&link, &cfg));
}

test "the staged credential copy is wiped after use" {
    var cfg = try filled("ra8-bench", "hunter22");
    var buf = sta.WireBuf{};
    buf.stage(&cfg);
    try std.testing.expectEqualStrings("hunter22", buf.pass[0..8]);
    buf.wipe();
    try std.testing.expect(std.mem.allEqual(u8, std.mem.asBytes(&buf), 0));
}
