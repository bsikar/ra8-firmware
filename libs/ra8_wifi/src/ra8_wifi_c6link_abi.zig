//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for `libs/ra8_wifi/inc/ra8_wifi_c6link.h`: the ESP32-C6
//! backend, one thin mapping from each `ra8_wifi` operation onto the
//! `ra8_c6link` station call that performs it, plus the event callback that
//! turns the co-processor's asynchronous connect/disconnect announcements into
//! the "associated / not associated" reading the facade asks for.
//!
//! Everything the facade exists to hide -- RPC ids, the transaction pump, the
//! interface index, `wifi_mode_t` -- is reached only from here. The judgement
//! this file owns (what an announcement latches, what the latches mean, where
//! the arena floor sits) lives in `internal/root.zig`.
//!
//! This is its own translation unit, and its own archive member, on purpose:
//! it is the only part of `ra8_wifi` that references `ra8_c6link`, so a target
//! that links the facade against a different backend must not be made to
//! resolve the radio stack's symbols.

const std = @import("std");
const implementation = @import("internal/root.zig");

/// Component tag on this backend's log lines, matching the C's `RA8_WIFI_C6_TAG`.
const tag: [*:0]const u8 = "WIFI-C6";

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;
extern fn ra8_secure_memzero(ptr: ?*anyopaque, len: usize) void;

/// Log a rejected pointer the way `RA8_CHECK_NULL_PTR` did, then answer
/// `k_ra8_err_null_ptr`.
fn nullPtr(message: [*:0]const u8) u16 {
    ra8_log_emit_error(tag, message);
    return implementation.err_null_ptr;
}

/// The link handle (`ra8_c6link_t`). Opaque here: this backend only ever
/// passes its address on, and the handle carries two DMA-aligned frame
/// buffers whose layout is `ra8_c6link`'s business.
pub const C6Link = opaque {};

/// A link-layer address on the `ra8_c6link` side (`ra8_c6link_mac_t`).
pub const C6Mac = extern struct {
    octet: [implementation.c6_mac_bytes]u8 = .{0} ** implementation.c6_mac_bytes,
};

/// The bound hardware seam (`ra8_c6link_transport_t`).
pub const Transport = extern struct {
    transfer: ?*const fn (
        ctx: ?*anyopaque,
        tx: ?[*]const u8,
        rx: ?[*]u8,
        len: u16,
    ) callconv(.c) u16 = null,
    handshake_active: ?*const fn (ctx: ?*anyopaque) callconv(.c) bool = null,
    delay_ms: ?*const fn (ctx: ?*anyopaque, ms: u16) callconv(.c) void = null,
    ctx: ?*anyopaque = null,
};

/// An announcement from the co-processor (`ra8_c6link_event_t`).
pub const Event = extern struct {
    kind: u8 = 0,
    ssid_len: u8 = 0,
    channel: u8 = 0,
    rssi: i8 = 0,
    reason: u16 = 0,
    wifi_event_id: i32 = 0,
    reset_reason: u32 = 0,
    bssid: C6Mac = .{},
    ssid: [implementation.c6_ssid_max + 1]u8 = .{0} ** (implementation.c6_ssid_max + 1),
};

/// Announcement sink (`ra8_c6link_event_cb_t`).
pub const EventCb = *const fn (ctx: ?*anyopaque, ev: ?*const Event) callconv(.c) void;
/// 802.3 receive sink (`ra8_c6link_rx_cb_t`).
pub const RxCb = *const fn (ctx: ?*anyopaque, frame: ?[*]const u8, len: u16) callconv(.c) void;

/// What `ra8_c6link_open` needs (`ra8_c6link_cfg_t`).
pub const LinkCfg = extern struct {
    transport: Transport = .{},
    arena: ?[*]u8 = null,
    arena_bytes: u32 = 0,
    event_cb: ?EventCb = null,
    rx_cb: ?RxCb = null,
    cb_ctx: ?*anyopaque = null,
};

/// Pump counters (`ra8_c6link_stats_t`).
pub const Stats = extern struct {
    transfers: u16 = 0,
    data: u16 = 0,
    idle: u16 = 0,
    bad_checksum: u16 = 0,
    malformed: u16 = 0,
    rpc_in: u16 = 0,
    events: u16 = 0,
    eth_in: u16 = 0,
    undecodable: u16 = 0,
    unrouted: u16 = 0,
    hs_timeouts: u16 = 0,
};

/// What the co-processor answers a liveness probe with (`ra8_c6link_fw_version_t`).
pub const FwVersion = extern struct {
    major: u32 = 0,
    minor: u32 = 0,
    patch: u32 = 0,
    chip_id: u32 = 0,
    target: [16]u8 = .{0} ** 16,
    target_len: u8 = 0,
};

/// Association request record (`ra8_c6link_sta_cfg_t`).
pub const StaCfg = extern struct {
    ssid: [implementation.c6_ssid_max + 1]u8 = .{0} ** (implementation.c6_ssid_max + 1),
    pass: [implementation.c6_pass_max + 1]u8 = .{0} ** (implementation.c6_pass_max + 1),
    bssid: C6Mac = .{},
    ssid_len: u8 = 0,
    pass_len: u8 = 0,
    channel: u8 = 0,
    bssid_set: bool = false,
};

/// What the co-processor knows about the associated AP (`ra8_c6link_ap_info_t`).
pub const ApInfo = extern struct {
    bssid: C6Mac = .{},
    ssid: [implementation.c6_ssid_max + 1]u8 = .{0} ** (implementation.c6_ssid_max + 1),
    ssid_len: u8 = 0,
    channel: u8 = 0,
    rssi: i8 = 0,
    authmode: i32 = 0,
};

/// Caller-allocated backend context (`ra8_wifi_c6link_t`).
///
/// `connected` and `disconnected` are written by the event callback, which
/// `ra8_c6link_poll` dispatches, and read by `service` right after that pump
/// returns. The C declared both `volatile`; every access here goes through a
/// volatile pointer, so the reads and writes are exactly as unelidable as the
/// C's were.
pub const Context = extern struct {
    link: ?*C6Link = null,
    transport: Transport = .{},
    arena: ?[*]u8 = null,
    arena_bytes: u32 = 0,
    rx_cb: ?RxCb = null,
    connected: bool = false,
    disconnected: bool = false,
    reason: u16 = 0,
};

/// What `ra8_wifi_c6link_setup` needs (`ra8_wifi_c6link_cfg_t`).
pub const SetupCfg = extern struct {
    link: ?*C6Link = null,
    transport: Transport = .{},
    arena: ?[*]u8 = null,
    arena_bytes: u32 = 0,
    rx_cb: ?RxCb = null,
};

comptime {
    const ptr = @sizeOf(usize);

    std.debug.assert(@sizeOf(C6Mac) == 6);
    std.debug.assert(@sizeOf(Transport) == ptr * 4);
    std.debug.assert(@offsetOf(Transport, "ctx") == ptr * 3);

    std.debug.assert(@offsetOf(Event, "reason") == 4);
    std.debug.assert(@offsetOf(Event, "wifi_event_id") == 8);
    std.debug.assert(@offsetOf(Event, "reset_reason") == 12);
    std.debug.assert(@offsetOf(Event, "bssid") == 16);
    std.debug.assert(@offsetOf(Event, "ssid") == 22);
    std.debug.assert(@sizeOf(Event) == 56);

    std.debug.assert(@offsetOf(LinkCfg, "arena") == ptr * 4);
    std.debug.assert(@offsetOf(LinkCfg, "arena_bytes") == ptr * 5);
    std.debug.assert(@offsetOf(LinkCfg, "event_cb") == ptr * 6);
    std.debug.assert(@offsetOf(LinkCfg, "rx_cb") == ptr * 7);
    std.debug.assert(@offsetOf(LinkCfg, "cb_ctx") == ptr * 8);
    std.debug.assert(@sizeOf(LinkCfg) == ptr * 9);

    std.debug.assert(@sizeOf(Stats) == 22);
    std.debug.assert(@offsetOf(FwVersion, "target") == 16);
    std.debug.assert(@offsetOf(FwVersion, "target_len") == 32);

    std.debug.assert(@offsetOf(StaCfg, "pass") == 33);
    std.debug.assert(@offsetOf(StaCfg, "bssid") == 98);
    std.debug.assert(@offsetOf(StaCfg, "ssid_len") == 104);
    std.debug.assert(@offsetOf(StaCfg, "bssid_set") == 107);
    std.debug.assert(@sizeOf(StaCfg) == 108);

    // The AP record's authmode is 4-aligned, so three bytes of padding sit
    // after `rssi` on both the host and Arm; written as alignment arithmetic
    // rather than a literal so a 64-bit host and a 32-bit target agree.
    std.debug.assert(@offsetOf(ApInfo, "ssid") == 6);
    std.debug.assert(@offsetOf(ApInfo, "ssid_len") == 39);
    std.debug.assert(@offsetOf(ApInfo, "authmode") == std.mem.alignForward(usize, 42, 4));
    std.debug.assert(@sizeOf(ApInfo) == 48);

    std.debug.assert(@offsetOf(Context, "transport") == ptr);
    std.debug.assert(@offsetOf(Context, "arena") == ptr * 5);
    std.debug.assert(@offsetOf(Context, "arena_bytes") == ptr * 6);
    std.debug.assert(@offsetOf(Context, "rx_cb") == ptr * 7);
    std.debug.assert(@offsetOf(Context, "connected") == ptr * 8);
    std.debug.assert(@offsetOf(Context, "disconnected") == ptr * 8 + 1);
    std.debug.assert(@offsetOf(Context, "reason") == ptr * 8 + 2);
    std.debug.assert(@sizeOf(Context) == std.mem.alignForward(usize, ptr * 8 + 4, ptr));

    std.debug.assert(@offsetOf(SetupCfg, "arena") == ptr * 5);
    std.debug.assert(@offsetOf(SetupCfg, "rx_cb") == ptr * 7);
    std.debug.assert(@sizeOf(SetupCfg) == ptr * 8);
}

extern fn ra8_c6link_open(link: ?*C6Link, cfg: ?*const LinkCfg) u16;
extern fn ra8_c6link_close(link: ?*C6Link) u16;
extern fn ra8_c6link_await_ready(link: ?*C6Link, max_transactions: u16, out: ?*FwVersion) u16;
extern fn ra8_c6link_poll(link: ?*C6Link, max_transactions: u16, stats: ?*Stats) u16;
extern fn ra8_c6link_sta_cfg_set(
    cfg: ?*StaCfg,
    ssid: ?[*:0]const u8,
    pass: ?[*:0]const u8,
) u16;
extern fn ra8_c6link_wifi_start(link: ?*C6Link) u16;
extern fn ra8_c6link_wifi_stop(link: ?*C6Link) u16;
extern fn ra8_c6link_wifi_join(link: ?*C6Link, cfg: ?*const StaCfg) u16;
extern fn ra8_c6link_wifi_leave(link: ?*C6Link) u16;
extern fn ra8_c6link_wifi_mac(link: ?*C6Link, out: ?*C6Mac) u16;
extern fn ra8_c6link_wifi_ap_info(link: ?*C6Link, out: ?*ApInfo) u16;

/// The backend context every row receives, or null when the caller passed one.
fn contextOf(ctx: ?*anyopaque) ?*Context {
    return @ptrCast(@alignCast(ctx));
}

/// Read the two station latches the event callback writes.
fn latchesOf(self: *Context) implementation.Latches {
    const connected: *volatile bool = &self.connected;
    const disconnected: *volatile bool = &self.disconnected;
    return .{
        .connected = connected.*,
        .disconnected = disconnected.*,
        .reason = self.reason,
    };
}

/// Write the two station latches back.
fn setLatches(self: *Context, latches: implementation.Latches) void {
    const connected: *volatile bool = &self.connected;
    const disconnected: *volatile bool = &self.disconnected;
    connected.* = latches.connected;
    disconnected.* = latches.disconnected;
    self.reason = latches.reason;
}

/// Latch a station event so `service` can report it.
///
/// Registered as the link's event callback. `ra8_c6link` guarantees a non-null
/// event and the context set at open, so no defensive guard is needed; a boot
/// or bare Wi-Fi announcement leaves the latches alone.
fn onEvent(ctx: ?*anyopaque, ev: ?*const Event) callconv(.c) void {
    const self = contextOf(ctx) orelse return;
    const event = ev orelse return;
    setLatches(self, latchesOf(self).latch(event.kind, event.reason));
}

/// Bring the link up: open it and prove the co-processor answers.
fn open(ctx: ?*anyopaque) callconv(.c) u16 {
    const self = contextOf(ctx) orelse return nullPtr("ctx");
    if (self.link == null) return nullPtr("ctx.link");

    var cfg: LinkCfg = .{};
    cfg.transport = self.transport;
    cfg.arena = self.arena;
    cfg.arena_bytes = self.arena_bytes;
    cfg.event_cb = onEvent;
    cfg.rx_cb = self.rx_cb;
    cfg.cb_ctx = self;

    const opened = ra8_c6link_open(self.link, &cfg);
    if (opened != implementation.err_ok) return opened;

    var fw: FwVersion = .{};
    return ra8_c6link_await_ready(self.link, implementation.c6_announce_transfers, &fw);
}

/// Release the link this backend opened. The transport stays up; whoever
/// brought it up owns that.
fn close(ctx: ?*anyopaque) callconv(.c) u16 {
    const self = contextOf(ctx) orelse return nullPtr("ctx");
    if (self.link == null) return nullPtr("ctx.link");
    return ra8_c6link_close(self.link);
}

/// Start the co-processor's radio in station mode.
fn radioUp(ctx: ?*anyopaque) callconv(.c) u16 {
    const self = contextOf(ctx) orelse return nullPtr("ctx");
    if (self.link == null) return nullPtr("ctx.link");
    return ra8_c6link_wifi_start(self.link);
}

/// Stop the radio and release the co-processor's Wi-Fi resources.
fn radioDown(ctx: ?*anyopaque) callconv(.c) u16 {
    const self = contextOf(ctx) orelse return nullPtr("ctx");
    if (self.link == null) return nullPtr("ctx.link");
    return ra8_c6link_wifi_stop(self.link);
}

/// Ask the station to associate with a network.
///
/// The latches are cleared first so a stale association cannot be mistaken for
/// this one, and the filled request record is zeroed on both exits: it carries
/// the passphrase.
fn join(ctx: ?*anyopaque, ssid: ?[*:0]const u8, psk: ?[*:0]const u8) callconv(.c) u16 {
    const self = contextOf(ctx) orelse return nullPtr("ctx");
    if (ssid == null) return nullPtr("ssid");

    setLatches(self, implementation.Latches.clear);

    var sta: StaCfg = .{};
    const set = ra8_c6link_sta_cfg_set(&sta, ssid, psk);
    if (set != implementation.err_ok) {
        ra8_secure_memzero(&sta, @sizeOf(StaCfg));
        return set;
    }
    const joined = ra8_c6link_wifi_join(self.link, &sta);
    ra8_secure_memzero(&sta, @sizeOf(StaCfg));
    return joined;
}

/// Disassociate the station from its current network.
fn leave(ctx: ?*anyopaque) callconv(.c) u16 {
    const self = contextOf(ctx) orelse return nullPtr("ctx");
    if (self.link == null) return nullPtr("ctx.link");
    return ra8_c6link_wifi_leave(self.link);
}

/// Service the link once and report whether the station is associated.
fn service(ctx: ?*anyopaque, out_link: ?*u8) callconv(.c) u16 {
    const self = contextOf(ctx) orelse return nullPtr("ctx");
    const sink = out_link orelse return nullPtr("out_link");

    var stats: Stats = .{};
    const err = ra8_c6link_poll(self.link, implementation.c6_announce_transfers, &stats);
    if (err != implementation.err_ok) return err;

    const latches = latchesOf(self);
    sink.* = @intFromEnum(implementation.linkForLatches(latches.connected, latches.disconnected));
    return implementation.err_ok;
}

/// Read the station's own MAC address.
fn getMac(ctx: ?*anyopaque, out: ?*implementation.Mac) callconv(.c) u16 {
    const self = contextOf(ctx) orelse return nullPtr("ctx");
    const sink = out orelse return nullPtr("out");

    var mac: C6Mac = .{};
    const err = ra8_c6link_wifi_mac(self.link, &mac);
    if (err != implementation.err_ok) return err;
    @memcpy(sink.octet[0..implementation.mac_bytes], mac.octet[0..implementation.c6_mac_bytes]);
    return implementation.err_ok;
}

/// Read what the co-processor knows about the associated AP.
fn getAp(ctx: ?*anyopaque, out: ?*implementation.Ap) callconv(.c) u16 {
    const self = contextOf(ctx) orelse return nullPtr("ctx");
    const sink = out orelse return nullPtr("out");

    var ap: ApInfo = .{};
    const err = ra8_c6link_wifi_ap_info(self.link, &ap);
    if (err != implementation.err_ok) return err;

    sink.* = .{};
    @memcpy(
        sink.bssid.octet[0..implementation.mac_bytes],
        ap.bssid.octet[0..implementation.c6_mac_bytes],
    );
    @memcpy(sink.ssid[0..], ap.ssid[0..]);
    sink.ssid_len = ap.ssid_len;
    sink.channel = ap.channel;
    sink.rssi = ap.rssi;
    sink.authmode = ap.authmode;
    return implementation.err_ok;
}

/// Idle for the requested milliseconds, on the transport's own clock.
///
/// The facade counts attempts and has no clock of its own; this is the seam
/// through which it paces one. A missing context or a transport with no delay
/// row is a no-op rather than a fault, because there is nothing to wait on.
fn idle(ctx: ?*anyopaque, ms: u16) callconv(.c) void {
    const self = contextOf(ctx) orelse return;
    const delay = self.transport.delay_ms orelse return;
    delay(self.transport.ctx, ms);
}

/// The one ESP32-C6 backend table; see the header for the contract.
pub export const k_ra8_wifi_backend_c6link: implementation.Backend = .{
    .open = open,
    .close = close,
    .radio_up = radioUp,
    .radio_down = radioDown,
    .join = join,
    .leave = leave,
    .service = service,
    .get_mac = getMac,
    .get_ap = getAp,
    .idle = idle,
};

pub export fn ra8_wifi_c6link_setup(
    self: ?*Context,
    cfg: ?*const SetupCfg,
    out_wcfg: ?*implementation.Config,
) u16 {
    const backend_ctx = self orelse return nullPtr("self");
    const config = cfg orelse return nullPtr("cfg");
    const out = out_wcfg orelse return nullPtr("out_wcfg");
    if (config.link == null) return nullPtr("cfg.link");
    if (implementation.arenaTooSmall(config.arena_bytes)) return implementation.err_invalid_size;

    backend_ctx.* = .{};
    backend_ctx.link = config.link;
    backend_ctx.transport = config.transport;
    backend_ctx.arena = config.arena;
    backend_ctx.arena_bytes = config.arena_bytes;
    backend_ctx.rx_cb = config.rx_cb;

    out.backend = &k_ra8_wifi_backend_c6link;
    out.backend_ctx = backend_ctx;
    return implementation.err_ok;
}

/// The pure core, re-exported so the ABI tests can build the exact struct
/// types this membrane pins.
pub const internal = implementation;
