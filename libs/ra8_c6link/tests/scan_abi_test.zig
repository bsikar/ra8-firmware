//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! `ra8_c6link_wifi_scan` against a scripted RPC layer and pump: the order of
//! the four steps, the wait on `Event_StaScanDone`, the record caps, and every
//! failure path clearing the output.

const std = @import("std");
const scan = @import("scan_abi");

const c = scan.c;

const Code = struct {
    pub const ok: u16 = 0;
    pub const invalid_arg: u16 = 0x103;
    pub const timeout: u16 = 0x108;
    pub const not_initialized: u16 = 0x10F;
    pub const hw_timeout: u16 = 0x203;
    pub const spi_error: u16 = 0x402;
    pub const protocol_error: u16 = 0x406;
    pub const null_ptr: u16 = 0x504;
};

const Script = struct {
    order: [4]u32 = [_]u32{0} ** 4,
    calls: usize = 0,
    pumps: usize = 0,
    done_after: usize = 1,
    pump_result: u16 = 0,
    fail_id: u32 = 0,
    verdict: u16 = 0,
    block: c_int = -1,
    config_set: i32 = -1,
    ap_num: i32 = 0,
    asked: i32 = -1,
    n_records: usize = 0,
    recs: [4]c.WifiApRecord = std.mem.zeroes([4]c.WifiApRecord),
    ptrs: [4][*c]c.WifiApRecord = [_][*c]c.WifiApRecord{null} ** 4,
};

var script: Script = .{};

export fn rpc__init(m: ?*c.Rpc) callconv(.c) void {
    m.?.* = std.mem.zeroes(c.Rpc);
}

export fn rpc__req__wifi_scan_start__init(m: ?*c.RpcReqWifiScanStart) callconv(.c) void {
    m.?.* = std.mem.zeroes(c.RpcReqWifiScanStart);
}

export fn rpc__req__wifi_scan_get_ap_num__init(m: ?*c.RpcReqWifiScanGetApNum) callconv(.c) void {
    m.?.* = std.mem.zeroes(c.RpcReqWifiScanGetApNum);
}

export fn rpc__req__wifi_scan_get_ap_records__init(m: ?*c.RpcReqWifiScanGetApRecords) callconv(.c) void {
    m.?.* = std.mem.zeroes(c.RpcReqWifiScanGetApRecords);
}

export fn priv_c6link_resp(link: ?*c.ra8_c6link_t, rpc_id: u32, resp: i32) callconv(.c) c.ra8_err_t {
    _ = .{ link, resp };
    return if (rpc_id == script.fail_id) script.verdict else Code.ok;
}

export fn priv_c6link_pump(link: ?*c.ra8_c6link_t, max: u16, stats: ?*c.ra8_c6link_stats_t) callconv(.c) u16 {
    _ = .{ max, stats };
    script.pumps += 1;
    if (script.done_after != 0 and script.pumps >= script.done_after) link.?.scan_done = true;
    return script.pump_result;
}

fn answer(r: *c.Rpc, resp: *c.Rpc, bodies: anytype) void {
    switch (r.msg_id) {
        c.RPC_ID__Req_WifiScanStart => {
            script.block = r.unnamed_0.req_wifi_scan_start.*.block;
            script.config_set = r.unnamed_0.req_wifi_scan_start.*.config_set;
            resp.unnamed_0.resp_wifi_scan_start = bodies.start;
        },
        c.RPC_ID__Req_WifiScanGetApNum => {
            bodies.num.number = script.ap_num;
            resp.unnamed_0.resp_wifi_scan_get_ap_num = bodies.num;
        },
        else => {
            script.asked = r.unnamed_0.req_wifi_scan_get_ap_records.*.number;
            bodies.recs.n_ap_records = script.n_records;
            bodies.recs.ap_records = &script.ptrs;
            resp.unnamed_0.resp_wifi_scan_get_ap_records = bodies.recs;
        },
    }
}

export fn priv_c6link_rpc_call(link: ?*c.ra8_c6link_t, req: ?*c.Rpc, resp_id: u32, take: c.ra8_c6link_take_fn_t, ctx: ?*anyopaque) callconv(.c) c.ra8_err_t {
    _ = .{ link, resp_id };
    const r = req.?;
    script.order[script.calls] = @intCast(r.msg_id);
    script.calls += 1;
    var start = std.mem.zeroes(c.RpcRespWifiScanStart);
    var num = std.mem.zeroes(c.RpcRespWifiScanGetApNum);
    var recs = std.mem.zeroes(c.RpcRespWifiScanGetApRecords);
    var resp = std.mem.zeroes(c.Rpc);
    answer(r, &resp, .{ .start = &start, .num = &num, .recs = &recs });
    return take.?(ctx, &resp);
}

var names = [_][8]u8{ "lab-ap-0".*, "lab-ap-1".*, "lab-ap-2".*, "lab-ap-3".* };
var macs = [_][6]u8{ .{ 2, 0, 0, 0, 0, 1 }, .{ 2, 0, 0, 0, 0, 2 }, .{ 2, 0, 0, 0, 0, 3 }, .{ 2, 0, 0, 0, 0, 4 } };

fn offer(announced: i32, sent: usize) void {
    script.ap_num = announced;
    script.n_records = sent;
    for (0..sent) |i| {
        script.recs[i].ssid = .{ .len = names[i].len, .data = &names[i] };
        script.recs[i].bssid = .{ .len = macs[i].len, .data = &macs[i] };
        script.recs[i].primary = 6;
        script.recs[i].rssi = -40 - @as(i32, @intCast(i));
        script.recs[i].authmode = 3;
        script.ptrs[i] = &script.recs[i];
    }
}

fn openLink() c.ra8_c6link_t {
    var link = std.mem.zeroes(c.ra8_c6link_t);
    link.open = true;
    return link;
}

const Run = struct { code: u16, count: u16, out: [4]c.ra8_c6link_ap_info_t };

fn scanInto(max: u16) Run {
    var link = openLink();
    var out: [4]c.ra8_c6link_ap_info_t = undefined;
    @memset(&out, std.mem.zeroes(c.ra8_c6link_ap_info_t));
    out[0].channel = 99;
    var count: u16 = 77;
    const code = scan.ra8_c6link_wifi_scan(&link, &out, max, &count);
    return .{ .code = code, .count = count, .out = out };
}

test "scan rejects null pointers, a zero max and a closed link before any request" {
    script = .{};
    var link = openLink();
    var out: [1]c.ra8_c6link_ap_info_t = undefined;
    var count: u16 = 5;
    try std.testing.expectEqual(Code.null_ptr, scan.ra8_c6link_wifi_scan(null, &out, 1, &count));
    try std.testing.expectEqual(Code.null_ptr, scan.ra8_c6link_wifi_scan(&link, null, 1, &count));
    try std.testing.expectEqual(Code.null_ptr, scan.ra8_c6link_wifi_scan(&link, &out, 1, null));
    try std.testing.expectEqual(Code.invalid_arg, scan.ra8_c6link_wifi_scan(&link, &out, 0, &count));
    try std.testing.expectEqual(@as(u16, 0), count);
    var closed = std.mem.zeroes(c.ra8_c6link_t);
    try std.testing.expectEqual(Code.not_initialized, scan.ra8_c6link_wifi_scan(&closed, &out, 1, &count));
    try std.testing.expectEqual(@as(usize, 0), script.calls);
}

test "scan starts non-blocking, waits for scan done, counts, then reads the records" {
    script = .{ .done_after = 2 };
    offer(2, 2);
    const got = scanInto(4);
    try std.testing.expectEqual(Code.ok, got.code);
    try std.testing.expectEqualSlices(u32, &.{ scan.Id.start, scan.Id.ap_num, scan.Id.records }, script.order[0..3]);
    try std.testing.expectEqual(@as(usize, 2), script.pumps);
    try std.testing.expectEqual(@as(c_int, 0), script.block);
    try std.testing.expectEqual(@as(i32, 0), script.config_set);
    try std.testing.expectEqual(@as(i32, 2), script.asked);
    try std.testing.expectEqual(@as(u16, 2), got.count);
    try std.testing.expectEqualStrings("lab-ap-1", std.mem.sliceTo(&got.out[1].ssid, 0));
    try std.testing.expectEqual(@as(u8, 8), got.out[1].ssid_len);
    try std.testing.expectEqual(@as(u8, 6), got.out[1].channel);
    try std.testing.expectEqual(@as(i8, -41), got.out[1].rssi);
    try std.testing.expectEqual(@as(i32, 3), got.out[1].authmode);
    try std.testing.expectEqual(@as(u8, 2), got.out[1].bssid.octet[5]);
}

test "more APs than max asks for max and caps a longer reply" {
    script = .{};
    offer(4, 4);
    const got = scanInto(2);
    try std.testing.expectEqual(Code.ok, got.code);
    try std.testing.expectEqual(@as(i32, 2), script.asked);
    try std.testing.expectEqual(@as(u16, 2), got.count);
    try std.testing.expectEqual(@as(u8, 0), got.out[2].ssid_len);
}

test "fewer records than announced reports what arrived" {
    script = .{};
    offer(3, 1);
    const got = scanInto(4);
    try std.testing.expectEqual(Code.ok, got.code);
    try std.testing.expectEqual(@as(i32, 3), script.asked);
    try std.testing.expectEqual(@as(u16, 1), got.count);
}

test "no APs found skips the records request" {
    script = .{};
    offer(0, 0);
    const got = scanInto(4);
    try std.testing.expectEqual(Code.ok, got.code);
    try std.testing.expectEqual(@as(usize, 2), script.calls);
    try std.testing.expectEqual(@as(u16, 0), got.count);
}

test "a refused start sends nothing else and does not pump" {
    script = .{ .fail_id = scan.Id.start, .verdict = Code.protocol_error };
    const got = scanInto(4);
    try std.testing.expectEqual(Code.protocol_error, got.code);
    try std.testing.expectEqual(@as(usize, 1), script.calls);
    try std.testing.expectEqual(@as(usize, 0), script.pumps);
    try std.testing.expectEqual(@as(u16, 0), got.count);
    try std.testing.expectEqual(@as(u8, 0), got.out[0].channel);
}

test "idle pump runs keep waiting; a scan that never finishes times out" {
    script = .{ .done_after = 3, .pump_result = Code.hw_timeout };
    offer(1, 1);
    try std.testing.expectEqual(Code.ok, scanInto(4).code);
    script = .{ .done_after = 0, .pump_result = Code.hw_timeout };
    const got = scanInto(4);
    try std.testing.expectEqual(Code.timeout, got.code);
    try std.testing.expectEqual(@as(usize, scan.done_rounds), script.pumps);
    try std.testing.expectEqual(@as(usize, 1), script.calls);
}

test "a bus fault while waiting ends the scan" {
    script = .{ .done_after = 0, .pump_result = Code.spi_error };
    const got = scanInto(4);
    try std.testing.expectEqual(Code.spi_error, got.code);
    try std.testing.expectEqual(@as(usize, 1), script.pumps);
}

test "a missing record is a protocol error and clears every record" {
    script = .{};
    offer(2, 2);
    script.ptrs[1] = null;
    const got = scanInto(4);
    try std.testing.expectEqual(Code.protocol_error, got.code);
    try std.testing.expectEqual(@as(u16, 0), got.count);
    try std.testing.expectEqual(@as(u8, 0), got.out[0].ssid_len);
}
