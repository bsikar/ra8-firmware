//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Tests for the C ABI membrane, driven through a mock backend table and a
//! mock IP provider exactly as `tests/wireless/src/test_ra8_wifi.c` drives the
//! archive. The point of the vtable is that the facade runs with nothing
//! behind it, so every guard and every backend-failure path is reachable by
//! choosing what the mock returns.
//!
//! The log sink is exported here so each rejected pointer can be checked by
//! the message the C's `RA8_CHECK_NULL_PTR` emitted for it.

const std = @import("std");
const abi = @import("abi");

const ok: u16 = 0x0000;
const err_invalid_state: u16 = 0x0104;
const err_timeout: u16 = 0x0108;
const err_not_initialized: u16 = 0x010F;
const err_hw_timeout: u16 = 0x0203;
const err_spi_error: u16 = 0x0402;
const err_protocol_error: u16 = 0x0406;
const err_null_ptr: u16 = 0x0504;

const join_polls: u16 = 200;
const poll_gap_ms: u16 = 50;

var log_count: usize = 0;
var log_last: [64]u8 = undefined;
var log_last_len: usize = 0;

export fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void {
    _ = tag;
    const text = std.mem.span(message);
    log_count += 1;
    log_last_len = @min(text.len, log_last.len);
    @memcpy(log_last[0..log_last_len], text[0..log_last_len]);
}

fn lastLog() []const u8 {
    return log_last[0..log_last_len];
}

/// What the mock backend did and what it should answer next, the same shape
/// the C suite's `t_mock_t` carries.
const Mock = struct {
    open_n: i32 = 0,
    close_n: i32 = 0,
    up_n: i32 = 0,
    down_n: i32 = 0,
    join_n: i32 = 0,
    leave_n: i32 = 0,
    service_n: i32 = 0,
    mac_n: i32 = 0,
    ap_n: i32 = 0,
    ip_n: i32 = 0,
    idle_n: i32 = 0,
    idle_ms: u16 = 0,

    open_ret: u16 = ok,
    close_ret: u16 = ok,
    up_ret: u16 = ok,
    down_ret: u16 = ok,
    join_ret: u16 = ok,
    leave_ret: u16 = ok,
    service_ret: u16 = ok,
    mac_ret: u16 = ok,
    ap_ret: u16 = ok,
    ip_ret: u16 = ok,

    service_fail_first: u16 = 0,
    link: u8 = 1,
    up_after: u16 = 0,
    mac: abi.Mac = .{ .octet = .{ 0x02, 0, 0, 0, 0, 0x2A } },
    ap: abi.Ap = .{},
    lease: abi.Lease = .{},
    last_ssid: [64]u8 = .{0} ** 64,
    last_ssid_len: usize = 0,
    saw_psk_null: bool = false,
};

var m: Mock = .{};

fn mockOpen(ctx: ?*anyopaque) callconv(.c) u16 {
    _ = ctx;
    m.open_n += 1;
    return m.open_ret;
}

fn mockClose(ctx: ?*anyopaque) callconv(.c) u16 {
    _ = ctx;
    m.close_n += 1;
    return m.close_ret;
}

fn mockRadioUp(ctx: ?*anyopaque) callconv(.c) u16 {
    _ = ctx;
    m.up_n += 1;
    return m.up_ret;
}

fn mockRadioDown(ctx: ?*anyopaque) callconv(.c) u16 {
    _ = ctx;
    m.down_n += 1;
    return m.down_ret;
}

fn mockJoin(ctx: ?*anyopaque, ssid: ?[*:0]const u8, psk: ?[*:0]const u8) callconv(.c) u16 {
    _ = ctx;
    m.join_n += 1;
    m.saw_psk_null = (psk == null);
    if (ssid) |s| {
        const text = std.mem.span(s);
        m.last_ssid_len = @min(text.len, m.last_ssid.len);
        @memcpy(m.last_ssid[0..m.last_ssid_len], text[0..m.last_ssid_len]);
    }
    return m.join_ret;
}

fn mockLeave(ctx: ?*anyopaque) callconv(.c) u16 {
    _ = ctx;
    m.leave_n += 1;
    return m.leave_ret;
}

fn mockService(ctx: ?*anyopaque, out_link: ?*u8) callconv(.c) u16 {
    _ = ctx;
    m.service_n += 1;
    // A radio busy on the air answers nothing for a while and then starts
    // answering: the silicon behaviour a permanent `service_ret` cannot express.
    if (@as(u16, @intCast(m.service_n)) <= m.service_fail_first) return err_hw_timeout;
    if (m.service_ret != ok) return m.service_ret;
    if (out_link) |sink| {
        sink.* = if (m.up_after != 0)
            @as(u8, if (@as(u16, @intCast(m.service_n)) >= m.up_after) 1 else 0)
        else
            m.link;
    }
    return ok;
}

fn mockGetMac(ctx: ?*anyopaque, out: ?*abi.Mac) callconv(.c) u16 {
    _ = ctx;
    m.mac_n += 1;
    if (m.mac_ret != ok) return m.mac_ret;
    if (out) |sink| sink.* = m.mac;
    return ok;
}

fn mockGetAp(ctx: ?*anyopaque, out: ?*abi.Ap) callconv(.c) u16 {
    _ = ctx;
    m.ap_n += 1;
    if (m.ap_ret != ok) return m.ap_ret;
    if (out) |sink| sink.* = m.ap;
    return ok;
}

fn mockIdle(ctx: ?*anyopaque, ms: u16) callconv(.c) void {
    _ = ctx;
    m.idle_n += 1;
    m.idle_ms = ms;
}

fn mockIpBind(ctx: ?*anyopaque, mac: ?*const abi.Mac, out: ?*abi.Lease) callconv(.c) u16 {
    _ = ctx;
    _ = mac;
    m.ip_n += 1;
    if (m.ip_ret != ok) return m.ip_ret;
    if (out) |sink| sink.* = m.lease;
    return ok;
}

const full_backend: abi.Backend = .{
    .open = mockOpen,
    .close = mockClose,
    .radio_up = mockRadioUp,
    .radio_down = mockRadioDown,
    .join = mockJoin,
    .leave = mockLeave,
    .service = mockService,
    .get_mac = mockGetMac,
    .get_ap = mockGetAp,
    .idle = mockIdle,
};

fn reset() void {
    m = .{};
    m.ap.rssi = -56;
    m.ap.channel = 6;
    m.ap.ssid_len = 3;
    m.ap.ssid[0] = 'r';
    m.ap.ssid[1] = 'a';
    m.ap.ssid[2] = '8';
    m.lease = .{
        .ip = 0xC0A80164,
        .mask = 0xFFFFFF00,
        .gateway = 0xC0A80101,
        .dhcp_server = 0xC0A801FE,
    };
    log_count = 0;
    log_last_len = 0;
}

fn config() abi.Config {
    return .{
        .backend = &full_backend,
        .backend_ctx = @ptrCast(&m),
        .ip_bind = mockIpBind,
        .ip_ctx = @ptrCast(&m),
    };
}

fn opened(wifi: *abi.Wifi) !void {
    try std.testing.expectEqual(ok, abi.ra8_wifi_init(wifi, &config()));
}

test "handle and vtable keep their C layout" {
    const ptr = @sizeOf(usize);
    try std.testing.expectEqual(ptr * 10, @sizeOf(abi.Backend));
    try std.testing.expectEqual(ptr * 4, @sizeOf(abi.Config));
    try std.testing.expectEqual(ptr * 4, @offsetOf(abi.Wifi, "mac"));
    try std.testing.expectEqual(ptr * 4 + 8, @offsetOf(abi.Wifi, "lease"));
    try std.testing.expectEqual(ptr * 4 + 28, @offsetOf(abi.Wifi, "state"));
}

test "init rejects a null handle and a null config" {
    reset();
    var wifi: abi.Wifi = .{};
    var cfg = config();
    try std.testing.expectEqual(err_null_ptr, abi.ra8_wifi_init(null, &cfg));
    try std.testing.expectEqualStrings("wifi", lastLog());
    try std.testing.expectEqual(err_null_ptr, abi.ra8_wifi_init(&wifi, null));
    try std.testing.expectEqualStrings("cfg", lastLog());
}

test "init rejects a config with no ip provider" {
    reset();
    var wifi: abi.Wifi = .{};
    var cfg = config();
    cfg.ip_bind = null;
    try std.testing.expectEqual(err_null_ptr, abi.ra8_wifi_init(&wifi, &cfg));
    try std.testing.expectEqualStrings("cfg.ip_bind", lastLog());
    try std.testing.expectEqual(@as(i32, 0), m.open_n);
}

test "init rejects a null backend table" {
    reset();
    var wifi: abi.Wifi = .{};
    var cfg = config();
    cfg.backend = null;
    try std.testing.expectEqual(err_null_ptr, abi.ra8_wifi_init(&wifi, &cfg));
    try std.testing.expectEqualStrings("backend", lastLog());
}

test "init dents every backend row in turn and names it" {
    const rows = [_]struct { name: []const u8, dent: *const fn (*abi.Backend) void }{
        .{ .name = "backend.open", .dent = struct {
            fn f(b: *abi.Backend) void {
                b.open = null;
            }
        }.f },
        .{ .name = "backend.close", .dent = struct {
            fn f(b: *abi.Backend) void {
                b.close = null;
            }
        }.f },
        .{ .name = "backend.radio_up", .dent = struct {
            fn f(b: *abi.Backend) void {
                b.radio_up = null;
            }
        }.f },
        .{ .name = "backend.radio_down", .dent = struct {
            fn f(b: *abi.Backend) void {
                b.radio_down = null;
            }
        }.f },
        .{ .name = "backend.join", .dent = struct {
            fn f(b: *abi.Backend) void {
                b.join = null;
            }
        }.f },
        .{ .name = "backend.leave", .dent = struct {
            fn f(b: *abi.Backend) void {
                b.leave = null;
            }
        }.f },
        .{ .name = "backend.service", .dent = struct {
            fn f(b: *abi.Backend) void {
                b.service = null;
            }
        }.f },
        .{ .name = "backend.get_mac", .dent = struct {
            fn f(b: *abi.Backend) void {
                b.get_mac = null;
            }
        }.f },
        .{ .name = "backend.get_ap", .dent = struct {
            fn f(b: *abi.Backend) void {
                b.get_ap = null;
            }
        }.f },
        .{ .name = "backend.idle", .dent = struct {
            fn f(b: *abi.Backend) void {
                b.idle = null;
            }
        }.f },
    };

    for (rows) |row| {
        reset();
        var wifi: abi.Wifi = .{};
        var dented = full_backend;
        row.dent(&dented);
        var cfg = config();
        cfg.backend = &dented;
        try std.testing.expectEqual(err_null_ptr, abi.ra8_wifi_init(&wifi, &cfg));
        try std.testing.expectEqualStrings(row.name, lastLog());
        // Nothing was opened while validation failed.
        try std.testing.expectEqual(@as(i32, 0), m.open_n);
    }
}

test "a backend open failure propagates and leaves the handle closed" {
    reset();
    var wifi: abi.Wifi = .{};
    m.open_ret = err_hw_timeout;
    try std.testing.expectEqual(err_hw_timeout, abi.ra8_wifi_init(&wifi, &config()));
    try std.testing.expectEqual(@as(i32, 1), m.open_n);
    try std.testing.expect(!wifi.open);
}

test "a second init on an open handle is refused" {
    reset();
    var wifi: abi.Wifi = .{};
    try opened(&wifi);
    try std.testing.expectEqual(@as(i32, 1), m.open_n);
    try std.testing.expectEqual(err_invalid_state, abi.ra8_wifi_init(&wifi, &config()));
    // The refusal did not re-open the transport.
    try std.testing.expectEqual(@as(i32, 1), m.open_n);
    var st: abi.Status = .{};
    try std.testing.expectEqual(ok, abi.ra8_wifi_status(&wifi, &st));
    try std.testing.expectEqual(@intFromEnum(abi.State.down), st.state);
}

test "deinit guards, surfaces the close result and stays closed" {
    reset();
    var wifi: abi.Wifi = .{};
    try std.testing.expectEqual(err_null_ptr, abi.ra8_wifi_deinit(null));
    try std.testing.expectEqual(err_not_initialized, abi.ra8_wifi_deinit(&wifi));
    try opened(&wifi);
    m.close_ret = err_spi_error;
    try std.testing.expectEqual(err_spi_error, abi.ra8_wifi_deinit(&wifi));
    try std.testing.expectEqual(@as(i32, 1), m.close_n);
    try std.testing.expectEqual(err_not_initialized, abi.ra8_wifi_deinit(&wifi));
}

test "connect associates and does not restart a radio already up" {
    reset();
    var wifi: abi.Wifi = .{};
    try opened(&wifi);
    m.up_after = 3;
    try std.testing.expectEqual(ok, abi.ra8_wifi_connect(&wifi, "ra8-bench", "secret"));
    try std.testing.expectEqual(@as(i32, 1), m.up_n);
    try std.testing.expectEqual(@as(i32, 1), m.mac_n);
    try std.testing.expectEqual(@as(i32, 1), m.join_n);
    try std.testing.expect(m.service_n >= 3);
    try std.testing.expect(!m.saw_psk_null);
    try std.testing.expectEqualStrings("ra8-bench", m.last_ssid[0..m.last_ssid_len]);

    var st: abi.Status = .{};
    try std.testing.expectEqual(ok, abi.ra8_wifi_status(&wifi, &st));
    try std.testing.expectEqual(@intFromEnum(abi.State.associated), st.state);
    try std.testing.expect(st.associated);
    try std.testing.expect(!st.ip_bound);

    m.up_after = 1;
    m.service_n = 0;
    try std.testing.expectEqual(ok, abi.ra8_wifi_connect(&wifi, "ra8-bench", null));
    try std.testing.expectEqual(@as(i32, 1), m.up_n);
    try std.testing.expect(m.saw_psk_null);
}

test "connect rides out a quiet radio and paces every failed attempt" {
    reset();
    var wifi: abi.Wifi = .{};
    try opened(&wifi);
    m.service_fail_first = 5;
    m.up_after = 6;
    try std.testing.expectEqual(ok, abi.ra8_wifi_connect(&wifi, "ra8-bench", "secret"));
    try std.testing.expectEqual(@as(i32, 6), m.service_n);
    try std.testing.expectEqual(@as(i32, 5), m.idle_n);
    try std.testing.expectEqual(poll_gap_ms, m.idle_ms);
}

test "connect guards and backend failures stop before the next step" {
    reset();
    var wifi: abi.Wifi = .{};
    try std.testing.expectEqual(err_null_ptr, abi.ra8_wifi_connect(null, "x", "y"));
    try std.testing.expectEqualStrings("wifi", lastLog());
    try std.testing.expectEqual(err_not_initialized, abi.ra8_wifi_connect(&wifi, "x", "y"));
    try opened(&wifi);
    try std.testing.expectEqual(err_null_ptr, abi.ra8_wifi_connect(&wifi, null, "y"));
    try std.testing.expectEqualStrings("ssid", lastLog());

    m.up_ret = err_hw_timeout;
    try std.testing.expectEqual(err_hw_timeout, abi.ra8_wifi_connect(&wifi, "x", "y"));
    try std.testing.expectEqual(@as(i32, 0), m.join_n);
    try std.testing.expect(!wifi.radio_on);

    m.up_ret = ok;
    m.mac_ret = err_protocol_error;
    try std.testing.expectEqual(err_protocol_error, abi.ra8_wifi_connect(&wifi, "x", "y"));
    try std.testing.expectEqual(@as(i32, 0), m.join_n);

    m.mac_ret = ok;
    m.join_ret = err_protocol_error;
    try std.testing.expectEqual(err_protocol_error, abi.ra8_wifi_connect(&wifi, "x", "y"));
}

test "a radio silent for the whole budget reports its own fault" {
    reset();
    var wifi: abi.Wifi = .{};
    try opened(&wifi);
    m.service_ret = err_spi_error;
    m.service_n = 0;
    try std.testing.expectEqual(err_spi_error, abi.ra8_wifi_connect(&wifi, "x", "y"));
    try std.testing.expectEqual(@as(i32, @intCast(join_polls)), m.service_n);
}

test "a radio that answers but never associates times out" {
    reset();
    var wifi: abi.Wifi = .{};
    try opened(&wifi);
    m.up_after = 0;
    m.link = 0;
    m.service_n = 0;
    try std.testing.expectEqual(err_timeout, abi.ra8_wifi_connect(&wifi, "x", "y"));
    try std.testing.expectEqual(@as(i32, @intCast(join_polls)), m.service_n);
    try std.testing.expectEqual(@as(i32, @intCast(join_polls)), m.idle_n);
}

test "wait_ip guards and refuses before association" {
    reset();
    var wifi: abi.Wifi = .{};
    try opened(&wifi);
    var lease: abi.Lease = .{};
    try std.testing.expectEqual(err_null_ptr, abi.ra8_wifi_wait_ip(null, &lease));
    try std.testing.expectEqual(err_null_ptr, abi.ra8_wifi_wait_ip(&wifi, null));
    try std.testing.expectEqualStrings("out", lastLog());
    try std.testing.expectEqual(err_invalid_state, abi.ra8_wifi_wait_ip(&wifi, &lease));
}

test "wait_ip surfaces a provider failure and a zero address as no lease" {
    reset();
    var wifi: abi.Wifi = .{};
    try opened(&wifi);
    m.up_after = 1;
    try std.testing.expectEqual(ok, abi.ra8_wifi_connect(&wifi, "x", "y"));

    var lease: abi.Lease = .{};
    m.ip_ret = err_timeout;
    try std.testing.expectEqual(err_timeout, abi.ra8_wifi_wait_ip(&wifi, &lease));
    try std.testing.expect(!lease.bound);

    m.ip_ret = ok;
    m.lease.ip = 0;
    try std.testing.expectEqual(err_timeout, abi.ra8_wifi_wait_ip(&wifi, &lease));
    try std.testing.expect(!lease.bound);
    try std.testing.expect(!wifi.lease.bound);
}

test "a real lease binds the handle and get_ip serves it without the provider" {
    reset();
    var wifi: abi.Wifi = .{};
    try opened(&wifi);
    m.up_after = 1;
    try std.testing.expectEqual(ok, abi.ra8_wifi_connect(&wifi, "x", "y"));

    var lease: abi.Lease = .{};
    try std.testing.expectEqual(ok, abi.ra8_wifi_wait_ip(&wifi, &lease));
    try std.testing.expect(lease.bound);
    try std.testing.expectEqual(@as(u32, 0xC0A80164), lease.ip);
    try std.testing.expectEqual(@as(u32, 0xC0A80101), lease.gateway);

    var st: abi.Status = .{};
    try std.testing.expectEqual(ok, abi.ra8_wifi_status(&wifi, &st));
    try std.testing.expectEqual(@intFromEnum(abi.State.ip_bound), st.state);
    try std.testing.expect(st.ip_bound);

    const before = m.ip_n;
    var cached: abi.Lease = .{};
    try std.testing.expectEqual(err_null_ptr, abi.ra8_wifi_get_ip(&wifi, null));
    try std.testing.expectEqual(ok, abi.ra8_wifi_get_ip(&wifi, &cached));
    try std.testing.expect(cached.bound);
    try std.testing.expectEqual(@as(u32, 0xC0A80164), cached.ip);
    try std.testing.expectEqual(before, m.ip_n);
}

test "get_ip guards on a closed handle" {
    reset();
    var wifi: abi.Wifi = .{};
    var lease: abi.Lease = .{};
    try std.testing.expectEqual(err_null_ptr, abi.ra8_wifi_get_ip(null, &lease));
    try std.testing.expectEqual(err_not_initialized, abi.ra8_wifi_get_ip(&wifi, &lease));
}

test "poll guards, surfaces service failures and tracks the link" {
    reset();
    var wifi: abi.Wifi = .{};
    var link: u8 = 1;
    try std.testing.expectEqual(err_null_ptr, abi.ra8_wifi_poll(null, &link));
    try std.testing.expectEqual(err_null_ptr, abi.ra8_wifi_poll(&wifi, null));
    try std.testing.expectEqual(err_not_initialized, abi.ra8_wifi_poll(&wifi, &link));
    // The out parameter is cleared before the state guards run.
    try std.testing.expectEqual(@as(u8, 0), link);

    try opened(&wifi);
    m.service_ret = err_spi_error;
    try std.testing.expectEqual(err_spi_error, abi.ra8_wifi_poll(&wifi, &link));

    m.service_ret = ok;
    m.up_after = 0;
    m.link = 1;
    try std.testing.expectEqual(ok, abi.ra8_wifi_poll(&wifi, &link));
    try std.testing.expectEqual(@as(u8, 1), link);
    try std.testing.expectEqual(@intFromEnum(abi.State.associated), wifi.state);

    m.link = 0;
    try std.testing.expectEqual(ok, abi.ra8_wifi_poll(&wifi, &link));
    try std.testing.expectEqual(@as(u8, 0), link);
    try std.testing.expectEqual(@intFromEnum(abi.State.down), wifi.state);
}

test "poll refuses once an IP is bound so it cannot pre-empt the IP stack" {
    reset();
    var wifi: abi.Wifi = .{};
    try opened(&wifi);
    m.link = 1;
    m.up_after = 1;
    try std.testing.expectEqual(ok, abi.ra8_wifi_connect(&wifi, "x", "y"));
    var lease: abi.Lease = .{};
    try std.testing.expectEqual(ok, abi.ra8_wifi_wait_ip(&wifi, &lease));
    var link: u8 = 1;
    try std.testing.expectEqual(err_invalid_state, abi.ra8_wifi_poll(&wifi, &link));
}

test "status guards" {
    reset();
    var wifi: abi.Wifi = .{};
    var st: abi.Status = .{};
    try std.testing.expectEqual(err_null_ptr, abi.ra8_wifi_status(null, &st));
    try std.testing.expectEqual(err_null_ptr, abi.ra8_wifi_status(&wifi, null));
    try std.testing.expectEqual(err_not_initialized, abi.ra8_wifi_status(&wifi, &st));
}

test "get_mac answers the radio, then falls back to the cached address" {
    reset();
    var wifi: abi.Wifi = .{};
    var mac: abi.Mac = abi.Mac.zero;
    try std.testing.expectEqual(err_null_ptr, abi.ra8_wifi_get_mac(null, &mac));
    try std.testing.expectEqual(err_not_initialized, abi.ra8_wifi_get_mac(&wifi, &mac));
    try opened(&wifi);
    try std.testing.expectEqual(err_null_ptr, abi.ra8_wifi_get_mac(&wifi, null));

    // Nothing has ever been read, so the backend's failure is the only honest
    // reply and the caller is handed an all-zero address.
    m.mac_ret = err_protocol_error;
    mac.octet[0] = 0xFF;
    try std.testing.expectEqual(err_protocol_error, abi.ra8_wifi_get_mac(&wifi, &mac));
    try std.testing.expectEqual(@as(u8, 0), mac.octet[0]);

    m.mac_ret = ok;
    try std.testing.expectEqual(ok, abi.ra8_wifi_get_mac(&wifi, &mac));
    try std.testing.expectEqual(@as(u8, 0x02), mac.octet[0]);
    try std.testing.expectEqual(@as(u8, 0x2A), mac.octet[5]);

    mac = abi.Mac.zero;
    m.mac_ret = err_spi_error;
    try std.testing.expectEqual(ok, abi.ra8_wifi_get_mac(&wifi, &mac));
    try std.testing.expectEqual(@as(u8, 0x02), mac.octet[0]);
    try std.testing.expectEqual(@as(u8, 0x2A), mac.octet[5]);
}

test "get_ap guards, clears on failure and caches the rssi into status" {
    reset();
    var wifi: abi.Wifi = .{};
    var ap: abi.Ap = .{};
    try std.testing.expectEqual(err_null_ptr, abi.ra8_wifi_get_ap(null, &ap));
    try std.testing.expectEqual(err_not_initialized, abi.ra8_wifi_get_ap(&wifi, &ap));
    try opened(&wifi);
    try std.testing.expectEqual(err_null_ptr, abi.ra8_wifi_get_ap(&wifi, null));
    try std.testing.expectEqual(err_invalid_state, abi.ra8_wifi_get_ap(&wifi, &ap));

    m.up_after = 1;
    try std.testing.expectEqual(ok, abi.ra8_wifi_connect(&wifi, "x", "y"));
    ap.channel = 11;
    m.ap_ret = err_protocol_error;
    try std.testing.expectEqual(err_protocol_error, abi.ra8_wifi_get_ap(&wifi, &ap));
    try std.testing.expectEqual(@as(u8, 0), ap.channel);

    m.ap_ret = ok;
    try std.testing.expectEqual(ok, abi.ra8_wifi_get_ap(&wifi, &ap));
    try std.testing.expectEqual(@as(i8, -56), ap.rssi);
    var st: abi.Status = .{};
    try std.testing.expectEqual(ok, abi.ra8_wifi_status(&wifi, &st));
    try std.testing.expectEqual(@as(i8, -56), st.rssi);
}

test "disconnect surfaces a leave failure but still tears the session down" {
    reset();
    var wifi: abi.Wifi = .{};
    try std.testing.expectEqual(err_null_ptr, abi.ra8_wifi_disconnect(null));
    try std.testing.expectEqual(err_not_initialized, abi.ra8_wifi_disconnect(&wifi));
    try opened(&wifi);
    m.up_after = 1;
    try std.testing.expectEqual(ok, abi.ra8_wifi_connect(&wifi, "x", "y"));
    var lease: abi.Lease = .{};
    try std.testing.expectEqual(ok, abi.ra8_wifi_wait_ip(&wifi, &lease));

    m.leave_ret = err_protocol_error;
    try std.testing.expectEqual(err_protocol_error, abi.ra8_wifi_disconnect(&wifi));
    try std.testing.expectEqual(@as(i32, 1), m.leave_n);
    try std.testing.expectEqual(@as(i32, 1), m.down_n);

    var st: abi.Status = .{};
    try std.testing.expectEqual(ok, abi.ra8_wifi_status(&wifi, &st));
    try std.testing.expectEqual(@intFromEnum(abi.State.down), st.state);
    try std.testing.expect(!st.ip_bound);
    var cleared: abi.Lease = .{};
    try std.testing.expectEqual(ok, abi.ra8_wifi_get_ip(&wifi, &cleared));
    try std.testing.expect(!cleared.bound);
}

test "a disconnected handle restarts the radio on the next connect" {
    reset();
    var wifi: abi.Wifi = .{};
    try opened(&wifi);
    m.up_after = 1;
    try std.testing.expectEqual(ok, abi.ra8_wifi_connect(&wifi, "x", "y"));
    try std.testing.expectEqual(ok, abi.ra8_wifi_disconnect(&wifi));
    m.service_n = 0;
    try std.testing.expectEqual(ok, abi.ra8_wifi_connect(&wifi, "x", "y"));
    try std.testing.expectEqual(@as(i32, 2), m.up_n);
}

test "a radio_down failure is surfaced when the leave succeeded" {
    reset();
    var wifi: abi.Wifi = .{};
    try opened(&wifi);
    m.up_after = 1;
    try std.testing.expectEqual(ok, abi.ra8_wifi_connect(&wifi, "x", "y"));
    m.down_ret = err_spi_error;
    try std.testing.expectEqual(err_spi_error, abi.ra8_wifi_disconnect(&wifi));
    try std.testing.expect(!wifi.radio_on);
}
