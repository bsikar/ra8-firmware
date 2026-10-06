//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Layouts and arithmetic for the ESP32-C6 wifi backend, the half that needs
//! no co-processor. The `ra8_c6link` structs are mirrored here because the
//! backend copies fields straight out of them, so their shapes are part of
//! this translation unit's contract; the comptime block pins every offset the
//! copies depend on.
//!
//! The link calls themselves live in `ra8_wifi_c6link_abi.zig`. Keeping the
//! flag folding, the arena floor and the two record copies here is what makes
//! them host-testable without a transport.

const std = @import("std");

/// The facade types, re-exported so a test binary reaches them through this
/// file rather than importing `internal/root.zig` as a second module.
pub const core = @import("root.zig");

/// Geometry and budgets this backend reads from `ra8_c6link.h`.
pub const c6 = struct {
    /// `k_ra8_c6link_mac_bytes`, and the facade's own MAC width.
    pub const mac_bytes: usize = 6;
    /// `k_ra8_c6link_ssid_max`, and the facade's own SSID capacity.
    pub const ssid_max: usize = 32;
    /// `k_ra8_c6link_pass_max`.
    pub const pass_max: usize = 64;
    /// `k_ra8_c6link_arena_min`: the smallest arena a link will open on.
    pub const arena_min: u32 = 2048;
    /// `k_ra8_c6link_announce_transfers`, the poll budget open and service use.
    pub const announce_transfers: u16 = 8;
};

comptime {
    // The straight copies below are only correct while the two libraries agree
    // on both widths, which is what the C asserted at the top of the file.
    std.debug.assert(c6.mac_bytes == core.mac_bytes);
    std.debug.assert(c6.ssid_max == core.ssid_max);
}

/// `ra8_c6link_mac_t`.
pub const Mac = extern struct {
    octet: [c6.mac_bytes]u8 = @splat(0),
};

/// `ra8_c6link_transport_t`: the SPI seam the link clocks through.
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

/// `ra8_c6link_rx_cb_t`: inbound ethernet frames, passed through untouched.
pub const RxCb = *const fn (ctx: ?*anyopaque, frame: ?[*]const u8, len: u16) callconv(.c) void;

/// `ra8_c6link_event_kind_t`.
pub const EventKind = enum(u8) {
    boot = 0,
    sta_connected = 1,
    sta_disconnected = 2,
    wifi = 3,
    _,
};

/// `ra8_c6link_event_t`.
pub const Event = extern struct {
    kind: EventKind = .boot,
    ssid_len: u8 = 0,
    channel: u8 = 0,
    rssi: i8 = 0,
    reason: u16 = 0,
    wifi_event_id: i32 = 0,
    reset_reason: u32 = 0,
    bssid: Mac = .{},
    ssid: [c6.ssid_max + 1]u8 = @splat(0),
};

/// `ra8_c6link_sta_cfg_t`. Held by value on the join stack and zeroed after.
pub const StaCfg = extern struct {
    ssid: [c6.ssid_max + 1]u8 = @splat(0),
    pass: [c6.pass_max + 1]u8 = @splat(0),
    bssid: Mac = .{},
    ssid_len: u8 = 0,
    pass_len: u8 = 0,
    channel: u8 = 0,
    bssid_set: bool = false,
};

/// `ra8_c6link_ap_info_t`: the record `get_ap` copies out of.
pub const ApInfo = extern struct {
    bssid: Mac = .{},
    ssid: [c6.ssid_max + 1]u8 = @splat(0),
    ssid_len: u8 = 0,
    channel: u8 = 0,
    rssi: i8 = 0,
    authmode: i32 = 0,
};

/// `ra8_c6link_stats_t`: filled by a poll, read by nobody here.
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

/// `ra8_c6link_fw_version_t`: read by `await_ready` and then discarded.
pub const FwVersion = extern struct {
    major: u32 = 0,
    minor: u32 = 0,
    patch: u32 = 0,
    chip_id: u32 = 0,
    target: [16]u8 = @splat(0),
    target_len: u8 = 0,
};

/// `ra8_c6link_cfg_t`, assembled by `open` from the handle's own fields.
pub const LinkCfg = extern struct {
    transport: Transport = .{},
    arena: ?[*]u8 = null,
    arena_bytes: u32 = 0,
    event_cb: ?*const fn (ctx: ?*anyopaque, ev: ?*const Event) callconv(.c) void = null,
    rx_cb: ?RxCb = null,
    cb_ctx: ?*anyopaque = null,
};

/// `ra8_wifi_c6link_cfg_t`: what a board hands `ra8_wifi_c6link_setup`.
pub const Cfg = extern struct {
    link: ?*anyopaque = null,
    transport: Transport = .{},
    arena: ?[*]u8 = null,
    arena_bytes: u32 = 0,
    rx_cb: ?RxCb = null,
};

/// `ra8_wifi_c6link_t`: the backend's own context, owned by the caller.
pub const Handle = extern struct {
    link: ?*anyopaque = null,
    transport: Transport = .{},
    arena: ?[*]u8 = null,
    arena_bytes: u32 = 0,
    rx_cb: ?RxCb = null,
    connected: bool = false,
    disconnected: bool = false,
    reason: u16 = 0,
};

comptime {
    const ptr = @sizeOf(*anyopaque);
    std.debug.assert(@sizeOf(Mac) == 6);
    std.debug.assert(@sizeOf(Transport) == ptr * 4);
    std.debug.assert(@sizeOf(Event) == 56);
    std.debug.assert(@offsetOf(Event, "reason") == 4);
    std.debug.assert(@offsetOf(Event, "wifi_event_id") == 8);
    std.debug.assert(@offsetOf(Event, "bssid") == 16);
    std.debug.assert(@offsetOf(Event, "ssid") == 22);
    std.debug.assert(@sizeOf(StaCfg) == 108);
    std.debug.assert(@offsetOf(StaCfg, "pass") == 33);
    std.debug.assert(@offsetOf(StaCfg, "bssid") == 98);
    std.debug.assert(@offsetOf(StaCfg, "bssid_set") == 107);
    std.debug.assert(@sizeOf(ApInfo) == @sizeOf(core.Ap));
    std.debug.assert(@offsetOf(ApInfo, "ssid") == @offsetOf(core.Ap, "ssid"));
    std.debug.assert(@offsetOf(ApInfo, "authmode") == @offsetOf(core.Ap, "authmode"));
    std.debug.assert(@sizeOf(Stats) == 22);
    std.debug.assert(@sizeOf(FwVersion) == 36);
    std.debug.assert(@offsetOf(Handle, "transport") == ptr);
    std.debug.assert(@offsetOf(Handle, "connected") == ptr * 8);
}

/// An arena under `k_ra8_c6link_arena_min` cannot hold the two frames the
/// handshake clocks, so `setup` refuses it rather than letting a decode fail
/// at run time.
pub fn arenaTooSmall(bytes: u32) bool {
    return bytes < c6.arena_min;
}

/// Rebuild the handle from the config a board supplied. Every other field,
/// the event latches included, starts from zero: a handle set up again must
/// not carry a connect or disconnect heard on its previous link into the next
/// `service` reading.
pub fn applyCfg(self: *Handle, cfg: *const Cfg) void {
    self.* = .{
        .link = cfg.link,
        .transport = cfg.transport,
        .arena = cfg.arena,
        .arena_bytes = cfg.arena_bytes,
        .rx_cb = cfg.rx_cb,
    };
}

/// Clear the association flags ahead of a join so a stale disconnect from the
/// previous attempt cannot decide this one.
pub fn armJoin(self: *Handle) void {
    self.connected = false;
    self.disconnected = false;
    self.reason = 0;
}

/// Fold one link event into the handle. Only the two station transitions say
/// anything about association; boot and the passthrough wifi events do not.
pub fn noteEvent(self: *Handle, ev: *const Event) void {
    if (ev.kind == .sta_connected) {
        self.connected = true;
        return;
    }
    if (ev.kind == .sta_disconnected) {
        self.disconnected = true;
        self.reason = ev.reason;
    }
}

/// Down unless a connect was seen, and a later disconnect wins outright. Two
/// single-condition tests in that order, so the precedence lives in the
/// sequence rather than in a compound decision owing MC/DC vectors.
pub fn linkState(self: *const Handle) core.Link {
    var link: core.Link = .down;
    if (self.connected) link = .up;
    if (self.disconnected) link = .down;
    return link;
}

/// `get_mac`'s copy: the station address, octet for octet.
pub fn copyMac(out: *core.Mac, mac: *const Mac) void {
    out.octet = mac.octet;
}

/// `get_ap`'s copy: the whole AP record, including the SSID's trailing NUL.
pub fn copyAp(out: *core.Ap, info: *const ApInfo) void {
    out.bssid.octet = info.bssid.octet;
    out.ssid = info.ssid;
    out.ssid_len = info.ssid_len;
    out.channel = info.channel;
    out.rssi = info.rssi;
    out.authmode = info.authmode;
}
