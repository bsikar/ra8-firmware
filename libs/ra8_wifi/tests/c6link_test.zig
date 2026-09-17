//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Tests for the ESP32-C6 backend membrane. The `ra8_c6link` stack is a set of
//! plain C symbols to this translation unit, so it is answered here by a fake
//! link recorded and scripted from the tests: that is exactly the seam
//! tests/wireless/src/test_ra8_wifi_c6link.c drives with the real stack and the
//! co-processor model, and it makes every guard, every mapping and every
//! failure arm reachable without a radio.

const std = @import("std");
const abi = @import("abi");

const ok: u16 = 0x0000;
const err_invalid_size: u16 = 0x0105;
const err_invalid_state: u16 = 0x0104;
const err_hw_timeout: u16 = 0x0203;
const err_spi_error: u16 = 0x0402;
const err_protocol_error: u16 = 0x0406;
const err_null_ptr: u16 = 0x0504;

const announce_transfers: u16 = 8;

// --- the log sink -----------------------------------------------------------

var log_count: usize = 0;
var log_last: [64]u8 = undefined;
var log_last_len: usize = 0;

export fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void {
    const name = std.mem.span(tag);
    std.debug.assert(std.mem.eql(u8, name, "WIFI-C6"));
    const text = std.mem.span(message);
    log_count += 1;
    log_last_len = @min(text.len, log_last.len);
    @memcpy(log_last[0..log_last_len], text[0..log_last_len]);
}

fn lastLog() []const u8 {
    return log_last[0..log_last_len];
}

var memzero_calls: usize = 0;
var memzero_bytes: usize = 0;

export fn ra8_secure_memzero(ptr: ?*anyopaque, len: usize) void {
    memzero_calls += 1;
    memzero_bytes = len;
    const bytes: [*]u8 = @ptrCast(ptr.?);
    @memset(bytes[0..len], 0);
}

// --- the fake ra8_c6link ----------------------------------------------------

/// What the fake link was asked to do and what it should answer next.
const Fake = struct {
    open_n: i32 = 0,
    close_n: i32 = 0,
    ready_n: i32 = 0,
    poll_n: i32 = 0,
    start_n: i32 = 0,
    stop_n: i32 = 0,
    join_n: i32 = 0,
    leave_n: i32 = 0,
    mac_n: i32 = 0,
    ap_n: i32 = 0,
    set_n: i32 = 0,
    delay_n: i32 = 0,
    last_delay_ms: u16 = 0,

    open_ret: u16 = ok,
    ready_ret: u16 = ok,
    poll_ret: u16 = ok,
    close_ret: u16 = ok,
    start_ret: u16 = ok,
    stop_ret: u16 = ok,
    join_ret: u16 = ok,
    leave_ret: u16 = ok,
    mac_ret: u16 = ok,
    ap_ret: u16 = ok,
    set_ret: u16 = ok,

    seen_cfg: abi.LinkCfg = .{},
    seen_ready_transfers: u16 = 0,
    seen_poll_transfers: u16 = 0,
    seen_link: ?*abi.C6Link = null,
    seen_sta: abi.StaCfg = .{},
    /// The passphrase bytes the fake saw, so the zeroing can be checked.
    seen_pass_len: usize = 0,

    mac: abi.C6Mac = .{ .octet = .{ 0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x11 } },
    ap: abi.ApInfo = .{},
};

var fake: Fake = .{};

/// Storage the tests hand out as the link handle. Nothing dereferences it here.
var link_storage: [8]u8 = .{0} ** 8;

fn fakeLink() *abi.C6Link {
    return @ptrCast(&link_storage);
}

export fn ra8_c6link_open(link: ?*abi.C6Link, cfg: ?*const abi.LinkCfg) u16 {
    fake.open_n += 1;
    fake.seen_link = link;
    fake.seen_cfg = cfg.?.*;
    return fake.open_ret;
}

export fn ra8_c6link_close(link: ?*abi.C6Link) u16 {
    fake.close_n += 1;
    fake.seen_link = link;
    return fake.close_ret;
}

export fn ra8_c6link_await_ready(
    link: ?*abi.C6Link,
    max_transactions: u16,
    out: ?*abi.FwVersion,
) u16 {
    fake.ready_n += 1;
    fake.seen_link = link;
    fake.seen_ready_transfers = max_transactions;
    out.?.* = .{ .major = 1, .minor = 2, .patch = 3 };
    return fake.ready_ret;
}

export fn ra8_c6link_poll(link: ?*abi.C6Link, max_transactions: u16, stats: ?*abi.Stats) u16 {
    fake.poll_n += 1;
    fake.seen_link = link;
    fake.seen_poll_transfers = max_transactions;
    stats.?.transfers = 1;
    return fake.poll_ret;
}

export fn ra8_c6link_sta_cfg_set(
    cfg: ?*abi.StaCfg,
    ssid: ?[*:0]const u8,
    pass: ?[*:0]const u8,
) u16 {
    fake.set_n += 1;
    if (fake.set_ret != ok) return fake.set_ret;
    const record = cfg.?;
    const name = std.mem.span(ssid.?);
    @memcpy(record.ssid[0..name.len], name);
    record.ssid_len = @intCast(name.len);
    if (pass) |secret| {
        const text = std.mem.span(secret);
        @memcpy(record.pass[0..text.len], text);
        record.pass_len = @intCast(text.len);
        fake.seen_pass_len = text.len;
    }
    return ok;
}

export fn ra8_c6link_wifi_start(link: ?*abi.C6Link) u16 {
    fake.start_n += 1;
    fake.seen_link = link;
    return fake.start_ret;
}

export fn ra8_c6link_wifi_stop(link: ?*abi.C6Link) u16 {
    fake.stop_n += 1;
    fake.seen_link = link;
    return fake.stop_ret;
}

export fn ra8_c6link_wifi_join(link: ?*abi.C6Link, cfg: ?*const abi.StaCfg) u16 {
    fake.join_n += 1;
    fake.seen_link = link;
    fake.seen_sta = cfg.?.*;
    return fake.join_ret;
}

export fn ra8_c6link_wifi_leave(link: ?*abi.C6Link) u16 {
    fake.leave_n += 1;
    fake.seen_link = link;
    return fake.leave_ret;
}

export fn ra8_c6link_wifi_mac(link: ?*abi.C6Link, out: ?*abi.C6Mac) u16 {
    fake.mac_n += 1;
    fake.seen_link = link;
    if (fake.mac_ret != ok) return fake.mac_ret;
    out.?.* = fake.mac;
    return ok;
}

export fn ra8_c6link_wifi_ap_info(link: ?*abi.C6Link, out: ?*abi.ApInfo) u16 {
    fake.ap_n += 1;
    fake.seen_link = link;
    if (fake.ap_ret != ok) return fake.ap_ret;
    out.?.* = fake.ap;
    return ok;
}

// --- the transport seam -----------------------------------------------------

fn transferStub(ctx: ?*anyopaque, tx: ?[*]const u8, rx: ?[*]u8, len: u16) callconv(.c) u16 {
    _ = ctx;
    _ = tx;
    _ = rx;
    _ = len;
    return ok;
}

fn handshakeStub(ctx: ?*anyopaque) callconv(.c) bool {
    _ = ctx;
    return true;
}

fn delayStub(ctx: ?*anyopaque, ms: u16) callconv(.c) void {
    _ = ctx;
    fake.delay_n += 1;
    fake.last_delay_ms = ms;
}

var transport_ctx: u32 = 0xA5A5A5A5;

fn boundTransport() abi.Transport {
    return .{
        .transfer = transferStub,
        .handshake_active = handshakeStub,
        .delay_ms = delayStub,
        .ctx = &transport_ctx,
    };
}

var arena: [4096]u8 = .{0} ** 4096;

fn setupCfg() abi.SetupCfg {
    return .{
        .link = fakeLink(),
        .transport = boundTransport(),
        .arena = &arena,
        .arena_bytes = arena.len,
        .rx_cb = null,
    };
}

/// A fresh fake and a backend context wired to it, the shape `t_up` builds in
/// the C suite.
fn wired(self: *abi.Context) abi.internal.Config {
    fake = .{};
    log_count = 0;
    log_last_len = 0;
    memzero_calls = 0;
    var wcfg: abi.internal.Config = .{};
    const cfg = setupCfg();
    std.debug.assert(abi.ra8_wifi_c6link_setup(self, &cfg, &wcfg) == ok);
    return wcfg;
}

const table = &abi.k_ra8_wifi_backend_c6link;

// --- setup ------------------------------------------------------------------

test "setup rejects each pointer in the C's order, naming it in the log" {
    var self: abi.Context = .{};
    var wcfg: abi.internal.Config = .{};
    const cfg = setupCfg();
    log_count = 0;

    try std.testing.expectEqual(err_null_ptr, abi.ra8_wifi_c6link_setup(null, &cfg, &wcfg));
    try std.testing.expectEqualStrings("self", lastLog());
    try std.testing.expectEqual(err_null_ptr, abi.ra8_wifi_c6link_setup(&self, null, &wcfg));
    try std.testing.expectEqualStrings("cfg", lastLog());
    try std.testing.expectEqual(err_null_ptr, abi.ra8_wifi_c6link_setup(&self, &cfg, null));
    try std.testing.expectEqualStrings("out_wcfg", lastLog());

    var no_link = cfg;
    no_link.link = null;
    try std.testing.expectEqual(err_null_ptr, abi.ra8_wifi_c6link_setup(&self, &no_link, &wcfg));
    try std.testing.expectEqualStrings("cfg.link", lastLog());
    try std.testing.expectEqual(@as(usize, 4), log_count);
}

test "setup refuses an arena under the link's floor, and logs nothing for it" {
    var self: abi.Context = .{};
    var wcfg: abi.internal.Config = .{};
    var tiny = setupCfg();
    tiny.arena_bytes = abi.internal.c6_arena_min - 1;
    log_count = 0;

    try std.testing.expectEqual(err_invalid_size, abi.ra8_wifi_c6link_setup(&self, &tiny, &wcfg));
    try std.testing.expectEqual(@as(usize, 0), log_count);
    // A refused setup leaves the facade configuration untouched.
    try std.testing.expect(wcfg.backend == null);
    try std.testing.expect(wcfg.backend_ctx == null);
}

test "setup selects this backend with the context it just filled" {
    var self: abi.Context = .{ .connected = true, .disconnected = true, .reason = 9 };
    var wcfg: abi.internal.Config = .{};
    const cfg = setupCfg();

    try std.testing.expectEqual(ok, abi.ra8_wifi_c6link_setup(&self, &cfg, &wcfg));
    try std.testing.expect(wcfg.backend == table);
    try std.testing.expect(wcfg.backend_ctx == @as(?*anyopaque, @ptrCast(&self)));
    try std.testing.expect(self.link == fakeLink());
    try std.testing.expectEqual(arena.len, self.arena_bytes);
    try std.testing.expect(self.transport.delay_ms == delayStub);
    // The latches are cleared by the assignment, whatever the caller left there.
    try std.testing.expect(!self.connected);
    try std.testing.expect(!self.disconnected);
    try std.testing.expectEqual(@as(u16, 0), self.reason);
}

test "setup leaves the IP half of the facade configuration alone" {
    var self: abi.Context = .{};
    var wcfg: abi.internal.Config = .{};
    var ip_ctx: u8 = 7;
    wcfg.ip_ctx = &ip_ctx;
    const cfg = setupCfg();

    try std.testing.expectEqual(ok, abi.ra8_wifi_c6link_setup(&self, &cfg, &wcfg));
    try std.testing.expect(wcfg.ip_ctx == @as(?*anyopaque, @ptrCast(&ip_ctx)));
    try std.testing.expect(wcfg.ip_bind == null);
}

test "every row of the exported table is filled" {
    try std.testing.expect(table.open != null);
    try std.testing.expect(table.close != null);
    try std.testing.expect(table.radio_up != null);
    try std.testing.expect(table.radio_down != null);
    try std.testing.expect(table.join != null);
    try std.testing.expect(table.leave != null);
    try std.testing.expect(table.service != null);
    try std.testing.expect(table.get_mac != null);
    try std.testing.expect(table.get_ap != null);
    try std.testing.expect(table.idle != null);
    try std.testing.expect(abi.internal.missingRow(.{}) == null);
}

// --- the guards -------------------------------------------------------------

test "a null context is refused by every row that answers" {
    var mac: abi.internal.Mac = .{ .octet = .{0} ** 6 };
    var ap: abi.internal.Ap = .{};
    var link: u8 = 0;
    fake = .{};

    try std.testing.expectEqual(err_null_ptr, table.open.?(null));
    try std.testing.expectEqual(err_null_ptr, table.close.?(null));
    try std.testing.expectEqual(err_null_ptr, table.radio_up.?(null));
    try std.testing.expectEqual(err_null_ptr, table.radio_down.?(null));
    try std.testing.expectEqual(err_null_ptr, table.join.?(null, "x", "y"));
    try std.testing.expectEqual(err_null_ptr, table.leave.?(null));
    try std.testing.expectEqual(err_null_ptr, table.service.?(null, &link));
    try std.testing.expectEqual(err_null_ptr, table.get_mac.?(null, &mac));
    try std.testing.expectEqual(err_null_ptr, table.get_ap.?(null, &ap));
    try std.testing.expectEqualStrings("ctx", lastLog());
    // Not one of them reached the link.
    try std.testing.expectEqual(@as(i32, 0), fake.open_n + fake.close_n + fake.poll_n);
}

test "a context with no link is refused by the rows that dereference it" {
    var no_link: abi.Context = .{};
    fake = .{};

    try std.testing.expectEqual(err_null_ptr, table.open.?(&no_link));
    try std.testing.expectEqual(err_null_ptr, table.close.?(&no_link));
    try std.testing.expectEqual(err_null_ptr, table.radio_up.?(&no_link));
    try std.testing.expectEqual(err_null_ptr, table.radio_down.?(&no_link));
    try std.testing.expectEqual(err_null_ptr, table.leave.?(&no_link));
    try std.testing.expectEqualStrings("ctx.link", lastLog());
    try std.testing.expectEqual(@as(i32, 0), fake.open_n);
    try std.testing.expectEqual(@as(i32, 0), fake.start_n);
}

test "a null out-parameter is refused before any wire" {
    var self: abi.Context = .{};
    _ = wired(&self);

    try std.testing.expectEqual(err_null_ptr, table.join.?(&self, null, null));
    try std.testing.expectEqualStrings("ssid", lastLog());
    try std.testing.expectEqual(err_null_ptr, table.service.?(&self, null));
    try std.testing.expectEqualStrings("out_link", lastLog());
    try std.testing.expectEqual(err_null_ptr, table.get_mac.?(&self, null));
    try std.testing.expectEqualStrings("out", lastLog());
    try std.testing.expectEqual(err_null_ptr, table.get_ap.?(&self, null));
    try std.testing.expectEqualStrings("out", lastLog());
    try std.testing.expectEqual(@as(i32, 0), fake.set_n + fake.poll_n + fake.mac_n + fake.ap_n);
}

// --- open -------------------------------------------------------------------

test "open hands the link this backend's own latch and context" {
    var self: abi.Context = .{};
    _ = wired(&self);

    try std.testing.expectEqual(ok, table.open.?(&self));
    try std.testing.expectEqual(@as(i32, 1), fake.open_n);
    try std.testing.expect(fake.seen_cfg.event_cb != null);
    try std.testing.expect(fake.seen_cfg.cb_ctx == @as(?*anyopaque, @ptrCast(&self)));
    try std.testing.expectEqual(arena.len, fake.seen_cfg.arena_bytes);
    try std.testing.expect(fake.seen_cfg.transport.transfer == transferStub);
    try std.testing.expect(fake.seen_cfg.rx_cb == null);
    // Liveness is established straight after, on the announcement budget.
    try std.testing.expectEqual(@as(i32, 1), fake.ready_n);
    try std.testing.expectEqual(announce_transfers, fake.seen_ready_transfers);
}

test "a refused open is returned without probing for liveness" {
    var self: abi.Context = .{};
    _ = wired(&self);
    fake.open_ret = err_invalid_state;

    try std.testing.expectEqual(err_invalid_state, table.open.?(&self));
    try std.testing.expectEqual(@as(i32, 1), fake.open_n);
    try std.testing.expectEqual(@as(i32, 0), fake.ready_n);
}

test "a co-processor that never answers surfaces from the liveness probe" {
    var self: abi.Context = .{};
    _ = wired(&self);
    fake.ready_ret = err_hw_timeout;

    try std.testing.expectEqual(err_hw_timeout, table.open.?(&self));
    try std.testing.expectEqual(@as(i32, 1), fake.ready_n);
}

// --- the thin mappings ------------------------------------------------------

test "close, radio up, radio down and leave are one link call each" {
    var self: abi.Context = .{};
    _ = wired(&self);

    try std.testing.expectEqual(ok, table.close.?(&self));
    try std.testing.expectEqual(ok, table.radio_up.?(&self));
    try std.testing.expectEqual(ok, table.radio_down.?(&self));
    try std.testing.expectEqual(ok, table.leave.?(&self));
    try std.testing.expectEqual(@as(i32, 1), fake.close_n);
    try std.testing.expectEqual(@as(i32, 1), fake.start_n);
    try std.testing.expectEqual(@as(i32, 1), fake.stop_n);
    try std.testing.expectEqual(@as(i32, 1), fake.leave_n);
    try std.testing.expect(fake.seen_link == fakeLink());
}

test "each mapping returns the link's own refusal unchanged" {
    var self: abi.Context = .{};
    _ = wired(&self);
    fake.close_ret = err_invalid_state;
    fake.start_ret = err_protocol_error;
    fake.stop_ret = err_protocol_error;
    fake.leave_ret = err_protocol_error;

    try std.testing.expectEqual(err_invalid_state, table.close.?(&self));
    try std.testing.expectEqual(err_protocol_error, table.radio_up.?(&self));
    try std.testing.expectEqual(err_protocol_error, table.radio_down.?(&self));
    try std.testing.expectEqual(err_protocol_error, table.leave.?(&self));
}

// --- join -------------------------------------------------------------------

test "join clears the latches so a stale association cannot be mistaken" {
    var self: abi.Context = .{};
    _ = wired(&self);
    self.connected = true;
    self.disconnected = true;
    self.reason = 11;

    try std.testing.expectEqual(ok, table.join.?(&self, "ra8-bench", "hunter2hunter2"));
    try std.testing.expect(!self.connected);
    try std.testing.expect(!self.disconnected);
    try std.testing.expectEqual(@as(u16, 0), self.reason);
    try std.testing.expectEqual(@as(i32, 1), fake.join_n);
    try std.testing.expectEqualStrings("ra8-bench", std.mem.sliceTo(&fake.seen_sta.ssid, 0));
}

test "join zeroes the credential record on the way out" {
    var self: abi.Context = .{};
    _ = wired(&self);

    try std.testing.expectEqual(ok, table.join.?(&self, "ra8-bench", "hunter2hunter2"));
    try std.testing.expectEqual(@as(usize, 1), memzero_calls);
    try std.testing.expectEqual(@sizeOf(abi.StaCfg), memzero_bytes);
    try std.testing.expectEqual(@as(usize, 14), fake.seen_pass_len);
}

test "a rejected credential record is returned without asking for a join" {
    var self: abi.Context = .{};
    _ = wired(&self);
    fake.set_ret = err_invalid_size;

    try std.testing.expectEqual(err_invalid_size, table.join.?(&self, "", "pw"));
    try std.testing.expectEqual(@as(i32, 0), fake.join_n);
    // The record is still wiped, and the latches are left clear for a retry.
    try std.testing.expectEqual(@as(usize, 1), memzero_calls);
    try std.testing.expect(!self.connected);
    try std.testing.expect(!self.disconnected);
}

test "a refused join surfaces and still wipes the record" {
    var self: abi.Context = .{};
    _ = wired(&self);
    fake.join_ret = err_protocol_error;

    try std.testing.expectEqual(err_protocol_error, table.join.?(&self, "ra8-bench", "pw"));
    try std.testing.expectEqual(@as(usize, 1), memzero_calls);
}

test "an open network joins with no passphrase at all" {
    var self: abi.Context = .{};
    _ = wired(&self);

    try std.testing.expectEqual(ok, table.join.?(&self, "ra8-open", null));
    try std.testing.expectEqual(@as(i32, 1), fake.join_n);
    try std.testing.expectEqual(@as(u8, 0), fake.seen_sta.pass_len);
}

// --- service and the event latch -------------------------------------------

test "service pumps once on the announcement budget and reports down" {
    var self: abi.Context = .{};
    _ = wired(&self);
    var link: u8 = @intFromEnum(abi.internal.Link.up);

    try std.testing.expectEqual(ok, table.service.?(&self, &link));
    try std.testing.expectEqual(@as(i32, 1), fake.poll_n);
    try std.testing.expectEqual(announce_transfers, fake.seen_poll_transfers);
    try std.testing.expectEqual(@intFromEnum(abi.internal.Link.down), link);
}

test "a connected announcement reaches service through the registered latch" {
    var self: abi.Context = .{};
    _ = wired(&self);
    try std.testing.expectEqual(ok, table.open.?(&self));
    const announce = fake.seen_cfg.event_cb.?;

    var boot: abi.Event = .{ .kind = @intFromEnum(abi.internal.EventKind.boot) };
    announce(fake.seen_cfg.cb_ctx, &boot);
    var link: u8 = @intFromEnum(abi.internal.Link.up);
    try std.testing.expectEqual(ok, table.service.?(&self, &link));
    try std.testing.expectEqual(@intFromEnum(abi.internal.Link.down), link);

    var up: abi.Event = .{ .kind = @intFromEnum(abi.internal.EventKind.sta_connected) };
    announce(fake.seen_cfg.cb_ctx, &up);
    try std.testing.expectEqual(ok, table.service.?(&self, &link));
    try std.testing.expectEqual(@intFromEnum(abi.internal.Link.up), link);

    var down: abi.Event = .{
        .kind = @intFromEnum(abi.internal.EventKind.sta_disconnected),
        .reason = 0x0F,
    };
    announce(fake.seen_cfg.cb_ctx, &down);
    try std.testing.expectEqual(ok, table.service.?(&self, &link));
    try std.testing.expectEqual(@intFromEnum(abi.internal.Link.down), link);
    try std.testing.expectEqual(@as(u16, 0x0F), self.reason);
}

test "a transport fault while servicing surfaces and writes no reading" {
    var self: abi.Context = .{};
    _ = wired(&self);
    self.connected = true;
    fake.poll_ret = err_spi_error;
    var link: u8 = 0xAA;

    try std.testing.expectEqual(err_spi_error, table.service.?(&self, &link));
    try std.testing.expectEqual(@as(u8, 0xAA), link);
}

// --- the two reads ----------------------------------------------------------

test "get_mac copies the six octets the link reported" {
    var self: abi.Context = .{};
    _ = wired(&self);
    var mac: abi.internal.Mac = .{ .octet = .{0} ** 6 };

    try std.testing.expectEqual(ok, table.get_mac.?(&self, &mac));
    try std.testing.expectEqualSlices(u8, &fake.mac.octet, &mac.octet);
}

test "a refused MAC read leaves the caller's address untouched" {
    var self: abi.Context = .{};
    _ = wired(&self);
    fake.mac_ret = err_protocol_error;
    var mac: abi.internal.Mac = .{ .octet = .{ 1, 2, 3, 4, 5, 6 } };

    try std.testing.expectEqual(err_protocol_error, table.get_mac.?(&self, &mac));
    try std.testing.expectEqualSlices(u8, &.{ 1, 2, 3, 4, 5, 6 }, &mac.octet);
}

test "get_ap maps the whole record into the facade type" {
    var self: abi.Context = .{};
    _ = wired(&self);
    fake.ap = .{
        .bssid = .{ .octet = .{ 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 } },
        .ssid_len = 9,
        .channel = 6,
        .rssi = -42,
        .authmode = 3,
    };
    @memcpy(fake.ap.ssid[0..9], "ra8-bench");
    var ap: abi.internal.Ap = .{};

    try std.testing.expectEqual(ok, table.get_ap.?(&self, &ap));
    try std.testing.expectEqualSlices(u8, &fake.ap.bssid.octet, &ap.bssid.octet);
    try std.testing.expectEqualStrings("ra8-bench", std.mem.sliceTo(&ap.ssid, 0));
    try std.testing.expectEqual(@as(u8, 9), ap.ssid_len);
    try std.testing.expectEqual(@as(u8, 6), ap.channel);
    try std.testing.expectEqual(@as(i8, -42), ap.rssi);
    try std.testing.expectEqual(@as(i32, 3), ap.authmode);
}

test "a refused AP read leaves the caller's record alone for the facade to clear" {
    var self: abi.Context = .{};
    _ = wired(&self);
    fake.ap_ret = err_protocol_error;
    var ap: abi.internal.Ap = .{ .channel = 11, .rssi = -7 };

    try std.testing.expectEqual(err_protocol_error, table.get_ap.?(&self, &ap));
    try std.testing.expectEqual(@as(u8, 11), ap.channel);
}

// --- idle -------------------------------------------------------------------

test "idle reaches the transport's own clock with the milliseconds asked for" {
    var self: abi.Context = .{};
    _ = wired(&self);

    table.idle.?(&self, abi.internal.poll_gap_ms);
    try std.testing.expectEqual(@as(i32, 1), fake.delay_n);
    try std.testing.expectEqual(abi.internal.poll_gap_ms, fake.last_delay_ms);
}

test "idle with nothing to wait on is a no-op, not a fault" {
    var self: abi.Context = .{};
    _ = wired(&self);
    var no_transport: abi.Context = .{};

    table.idle.?(null, 1);
    table.idle.?(&no_transport, 1);
    try std.testing.expectEqual(@as(i32, 0), fake.delay_n);
    // And it logs nothing: a void row has no way to report a rejection.
    try std.testing.expectEqual(@as(usize, 0), log_count);
}
