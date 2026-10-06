//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the station scan: `ra8_c6link_wifi_scan`, as `ra8_c6link_wifi.h`
//! declares it. Four steps, in the order the co-processor expects them:
//! `Req_WifiScanStart` (non-blocking, default all-channel configuration),
//! a wait for `Event_StaScanDone`, `Req_WifiScanGetApNum`, then
//! `Req_WifiScanGetApRecords` for as many records as the caller has room for.
//!
//! The scan starts non-blocking so the RPC slot is not held for the whole
//! channel dwell, and so the end of the scan arrives as the event the bench
//! capture for the emulator's C6 model has to show.

const std = @import("std");
const Err = @import("abi_err.zig");
const record = @import("ra8_c6link_ap_record.zig");
const header = @import("c6link_rpc_c.zig");

/// The private `ra8_c6link_internal.h` view, codec types included.
pub const c = header.c;

/// The ids the scan is issued and answered under.
pub const Id = struct {
    pub const start: u32 = c.RPC_ID__Req_WifiScanStart;
    pub const start_resp: u32 = c.RPC_ID__Resp_WifiScanStart;
    pub const ap_num: u32 = c.RPC_ID__Req_WifiScanGetApNum;
    pub const ap_num_resp: u32 = c.RPC_ID__Resp_WifiScanGetApNum;
    pub const records: u32 = c.RPC_ID__Req_WifiScanGetApRecords;
    pub const records_resp: u32 = c.RPC_ID__Resp_WifiScanGetApRecords;
};

/// Pump runs spent waiting for `Event_StaScanDone`. Each run is bounded the
/// same way one request's wait is, so the scan gets eight request waits: an
/// active scan dwells on every channel in turn and takes far longer than one
/// RPC round trip.
pub const done_rounds: u8 = 8;

/// Where the record extractor writes, and how much room it has.
const Records = struct {
    out: [*]c.ra8_c6link_ap_info_t,
    max: u16,
    count: u16 = 0,
};

fn view(ctx: ?*anyopaque, msg_v: ?*const anyopaque) struct { *c.ra8_c6link_take_ctx_t, *const c.Rpc } {
    return .{ @ptrCast(@alignCast(ctx.?)), @ptrCast(@alignCast(msg_v.?)) };
}

/// `Resp_WifiScanStart` carries only a verdict.
fn takeStart(ctx: ?*anyopaque, msg_v: ?*const anyopaque) callconv(.c) c.ra8_err_t {
    const take, const msg = view(ctx, msg_v);
    const body: *const c.RpcRespWifiScanStart = msg.unnamed_0.resp_wifi_scan_start orelse return Err.protocol_error;
    return c.priv_c6link_resp(take.link, take.rpc_id, body.resp);
}

/// `Resp_WifiScanGetApNum`: how many APs the scan found. A negative count is
/// a malformed answer, not an empty scan.
fn takeNum(ctx: ?*anyopaque, msg_v: ?*const anyopaque) callconv(.c) c.ra8_err_t {
    const take, const msg = view(ctx, msg_v);
    const out: *u16 = @ptrCast(@alignCast(take.out.?));
    const body: *const c.RpcRespWifiScanGetApNum = msg.unnamed_0.resp_wifi_scan_get_ap_num orelse return Err.protocol_error;
    const reported = c.priv_c6link_resp(take.link, take.rpc_id, body.resp);
    if (reported != Err.ok) return reported;
    if (body.number < 0) return Err.protocol_error;
    out.* = @intCast(@min(body.number, std.math.maxInt(u16)));
    return Err.ok;
}

/// `Resp_WifiScanGetApRecords`: copy at most `max` records. A reply longer
/// than asked for is capped; a shorter one leaves `count` at what arrived.
fn takeRecords(ctx: ?*anyopaque, msg_v: ?*const anyopaque) callconv(.c) c.ra8_err_t {
    const take, const msg = view(ctx, msg_v);
    const recs: *Records = @ptrCast(@alignCast(take.out.?));
    const body: *const c.RpcRespWifiScanGetApRecords =
        msg.unnamed_0.resp_wifi_scan_get_ap_records orelse return Err.protocol_error;
    const reported = c.priv_c6link_resp(take.link, take.rpc_id, body.resp);
    if (reported != Err.ok) return reported;
    const n = @min(body.n_ap_records, recs.max);
    if (n != 0 and body.ap_records == null) return Err.protocol_error;
    for (0..n) |i| {
        const rec: *const c.WifiApRecord = body.ap_records[i] orelse return Err.protocol_error;
        record.fill(&recs.out[i], rec);
        recs.count += 1;
    }
    return Err.ok;
}

/// One request through `priv_c6link_rpc_call`, extracted by `take_fn`.
fn call(link: *c.ra8_c6link_t, rpc: *c.Rpc, ids: [2]u32, take_fn: c.ra8_c6link_take_fn_t, out: ?*anyopaque) c.ra8_err_t {
    var take = c.ra8_c6link_take_ctx_t{ .link = link, .out = out, .rpc_id = ids[0] };
    return c.priv_c6link_rpc_call(link, rpc, ids[1], take_fn, &take);
}

fn envelope(rpc: *c.Rpc, id: u32, payload: c_uint) void {
    c.rpc__init(rpc);
    rpc.msg_type = c.RPC_TYPE__Req;
    rpc.msg_id = id;
    rpc.payload_case = payload;
}

/// Start a non-blocking scan with the co-processor's default configuration.
fn start(link: *c.ra8_c6link_t) c.ra8_err_t {
    var body: c.RpcReqWifiScanStart = undefined;
    c.rpc__req__wifi_scan_start__init(&body);
    body.block = 0;
    body.config_set = 0;
    var rpc: c.Rpc = undefined;
    envelope(&rpc, Id.start, c.RPC__PAYLOAD_REQ_WIFI_SCAN_START);
    rpc.unnamed_0.req_wifi_scan_start = &body;
    return call(link, &rpc, .{ Id.start, Id.start_resp }, takeStart, null);
}

/// Pump until the event decoder latches `scan_done`. A run that clocked
/// nothing is the co-processor still scanning, not a failure; a bus fault is.
fn awaitDone(link: *c.ra8_c6link_t) c.ra8_err_t {
    var rounds: u8 = 0;
    while (!link.scan_done) : (rounds += 1) {
        if (rounds == done_rounds) {
            link.fault.rpc_id = @intCast(Id.start);
            link.fault.resp = 0;
            return Err.timeout;
        }
        var stats = std.mem.zeroes(c.ra8_c6link_stats_t);
        const pumped = c.priv_c6link_pump(link, @intCast(c.k_ra8_c6link_rpc_transfers), &stats);
        if (pumped != Err.ok and pumped != Err.hw_timeout) return pumped;
    }
    return Err.ok;
}

fn apNum(link: *c.ra8_c6link_t, out: *u16) c.ra8_err_t {
    var body: c.RpcReqWifiScanGetApNum = undefined;
    c.rpc__req__wifi_scan_get_ap_num__init(&body);
    var rpc: c.Rpc = undefined;
    envelope(&rpc, Id.ap_num, c.RPC__PAYLOAD_REQ_WIFI_SCAN_GET_AP_NUM);
    rpc.unnamed_0.req_wifi_scan_get_ap_num = &body;
    return call(link, &rpc, .{ Id.ap_num, Id.ap_num_resp }, takeNum, out);
}

fn fetch(link: *c.ra8_c6link_t, recs: *Records) c.ra8_err_t {
    var body: c.RpcReqWifiScanGetApRecords = undefined;
    c.rpc__req__wifi_scan_get_ap_records__init(&body);
    body.number = recs.max;
    var rpc: c.Rpc = undefined;
    envelope(&rpc, Id.records, c.RPC__PAYLOAD_REQ_WIFI_SCAN_GET_AP_RECORDS);
    rpc.unnamed_0.req_wifi_scan_get_ap_records = &body;
    return call(link, &rpc, .{ Id.records, Id.records_resp }, takeRecords, recs);
}

/// `ra8_c6link_wifi_scan`: scan, then read up to `max` AP records into `out`.
/// `count` and `out` are cleared first and again on any failure, so a failed
/// scan never leaves a previous result behind.
pub export fn ra8_c6link_wifi_scan(
    link: ?*c.ra8_c6link_t,
    out: ?[*]c.ra8_c6link_ap_info_t,
    max: u16,
    count: ?*u16,
) callconv(.c) c.ra8_err_t {
    const handle = link orelse return Err.null_ptr;
    const records = out orelse return Err.null_ptr;
    const found = count orelse return Err.null_ptr;
    found.* = 0;
    if (max == 0) return Err.invalid_arg;
    if (!handle.open) return Err.not_initialized;
    const dst = records[0..max];
    @memset(dst, std.mem.zeroes(c.ra8_c6link_ap_info_t));

    var recs = Records{ .out = records, .max = max };
    const result = run(handle, &recs);
    if (result != Err.ok) {
        @memset(dst, std.mem.zeroes(c.ra8_c6link_ap_info_t));
        return result;
    }
    found.* = recs.count;
    return Err.ok;
}

/// Start, wait, count, then read `min(max, announced)` records.
fn run(link: *c.ra8_c6link_t, recs: *Records) c.ra8_err_t {
    link.scan_done = false;
    const started = start(link);
    if (started != Err.ok) return started;
    const waited = awaitDone(link);
    if (waited != Err.ok) return waited;
    var announced: u16 = 0;
    const counted = apNum(link, &announced);
    if (counted != Err.ok) return counted;
    recs.max = @min(recs.max, announced);
    if (recs.max == 0) return Err.ok;
    return fetch(link, recs);
}
