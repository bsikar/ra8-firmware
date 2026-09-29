//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for `libs/ra8_wifi/inc/ra8_wifi_c6link.h`: the ESP32-C6
//! backend rows the wifi facade dispatches through, plus the setup call a
//! board uses to point a handle at its link.
//!
//! Every row here is a thin delegation to `ra8_c6link`, which is still C. The
//! arithmetic that does not need the co-processor -- the arena floor, the
//! association flags, the two record copies -- lives in `internal/c6link.zig`
//! so the host tests can reach it without a transport.

const std = @import("std");
const implementation = @import("internal/c6link.zig");
const core = implementation.core;
const facade = @import("ra8_wifi_abi.zig");

/// Component tag on this backend's log lines, matching `RA8_WIFI_C6_TAG`.
const tag: [*:0]const u8 = "WIFI-C6";

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;
extern fn ra8_secure_memzero(ptr: ?*anyopaque, len: usize) void;

extern fn ra8_c6link_open(link: ?*anyopaque, cfg: ?*const implementation.LinkCfg) u16;
extern fn ra8_c6link_close(link: ?*anyopaque) u16;
extern fn ra8_c6link_poll(
    link: ?*anyopaque,
    max_transactions: u16,
    stats: ?*implementation.Stats,
) u16;
extern fn ra8_c6link_await_ready(
    link: ?*anyopaque,
    max_transactions: u16,
    out: ?*implementation.FwVersion,
) u16;
extern fn ra8_c6link_sta_cfg_set(
    cfg: ?*implementation.StaCfg,
    ssid: ?[*:0]const u8,
    pass: ?[*:0]const u8,
) u16;
extern fn ra8_c6link_wifi_start(link: ?*anyopaque) u16;
extern fn ra8_c6link_wifi_stop(link: ?*anyopaque) u16;
extern fn ra8_c6link_wifi_join(link: ?*anyopaque, cfg: ?*const implementation.StaCfg) u16;
extern fn ra8_c6link_wifi_leave(link: ?*anyopaque) u16;
extern fn ra8_c6link_wifi_mac(link: ?*anyopaque, out: ?*implementation.Mac) u16;
extern fn ra8_c6link_wifi_ap_info(link: ?*anyopaque, out: ?*implementation.ApInfo) u16;

/// Log a rejected pointer the way `RA8_CHECK_NULL_PTR` did, then answer
/// `k_ra8_err_null_ptr`.
fn nullPtr(message: [*:0]const u8) u16 {
    ra8_log_emit_error(tag, message);
    return core.err_null_ptr;
}

/// `k_ra8_err_invalid_size`, the answer to an arena under the link's floor.
const err_invalid_size: u16 = 0x0105;

/// The event sink the handle registers on its link, so an association change
/// reaches the flags `service` reads.
fn onEvent(ctx: ?*anyopaque, ev: ?*const implementation.Event) callconv(.c) void {
    const self: *implementation.Handle = @ptrCast(@alignCast(ctx orelse return));
    implementation.noteEvent(self, ev orelse return);
}

fn opOpen(ctx: ?*anyopaque) callconv(.c) u16 {
    const self: *implementation.Handle = @ptrCast(@alignCast(ctx orelse return nullPtr("ctx")));
    if (self.link == null) return nullPtr("ctx.link");

    const cfg: implementation.LinkCfg = .{
        .transport = self.transport,
        .arena = self.arena,
        .arena_bytes = self.arena_bytes,
        .event_cb = &onEvent,
        .rx_cb = self.rx_cb,
        .cb_ctx = self,
    };
    const opened = ra8_c6link_open(self.link, &cfg);
    if (opened != core.err_ok) return opened;

    var fw: implementation.FwVersion = .{};
    return ra8_c6link_await_ready(self.link, implementation.c6.announce_transfers, &fw);
}

fn opClose(ctx: ?*anyopaque) callconv(.c) u16 {
    const self: *implementation.Handle = @ptrCast(@alignCast(ctx orelse return nullPtr("ctx")));
    if (self.link == null) return nullPtr("ctx.link");
    return ra8_c6link_close(self.link);
}

fn opRadioUp(ctx: ?*anyopaque) callconv(.c) u16 {
    const self: *implementation.Handle = @ptrCast(@alignCast(ctx orelse return nullPtr("ctx")));
    if (self.link == null) return nullPtr("ctx.link");
    return ra8_c6link_wifi_start(self.link);
}

fn opRadioDown(ctx: ?*anyopaque) callconv(.c) u16 {
    const self: *implementation.Handle = @ptrCast(@alignCast(ctx orelse return nullPtr("ctx")));
    if (self.link == null) return nullPtr("ctx.link");
    return ra8_c6link_wifi_stop(self.link);
}

fn opJoin(ctx: ?*anyopaque, ssid: ?[*:0]const u8, psk: ?[*:0]const u8) callconv(.c) u16 {
    const self: *implementation.Handle = @ptrCast(@alignCast(ctx orelse return nullPtr("ctx")));
    const name = ssid orelse return nullPtr("ssid");

    implementation.armJoin(self);

    var sta: implementation.StaCfg = .{};
    const set = ra8_c6link_sta_cfg_set(&sta, name, psk);
    if (set != core.err_ok) {
        ra8_secure_memzero(&sta, @sizeOf(implementation.StaCfg));
        return set;
    }
    const joined = ra8_c6link_wifi_join(self.link, &sta);
    ra8_secure_memzero(&sta, @sizeOf(implementation.StaCfg));
    return joined;
}

fn opLeave(ctx: ?*anyopaque) callconv(.c) u16 {
    const self: *implementation.Handle = @ptrCast(@alignCast(ctx orelse return nullPtr("ctx")));
    if (self.link == null) return nullPtr("ctx.link");
    return ra8_c6link_wifi_leave(self.link);
}

fn opService(ctx: ?*anyopaque, out_link: ?*u8) callconv(.c) u16 {
    const self: *implementation.Handle = @ptrCast(@alignCast(ctx orelse return nullPtr("ctx")));
    const sink = out_link orelse return nullPtr("out_link");

    var stats: implementation.Stats = .{};
    const err = ra8_c6link_poll(self.link, implementation.c6.announce_transfers, &stats);
    if (err != core.err_ok) return err;

    sink.* = @intFromEnum(implementation.linkState(self));
    return core.err_ok;
}

fn opGetMac(ctx: ?*anyopaque, out: ?*core.Mac) callconv(.c) u16 {
    const self: *implementation.Handle = @ptrCast(@alignCast(ctx orelse return nullPtr("ctx")));
    const sink = out orelse return nullPtr("out");

    var mac: implementation.Mac = .{};
    const err = ra8_c6link_wifi_mac(self.link, &mac);
    if (err != core.err_ok) return err;

    implementation.copyMac(sink, &mac);
    return core.err_ok;
}

fn opGetAp(ctx: ?*anyopaque, out: ?*core.Ap) callconv(.c) u16 {
    const self: *implementation.Handle = @ptrCast(@alignCast(ctx orelse return nullPtr("ctx")));
    const sink = out orelse return nullPtr("out");

    var ap: implementation.ApInfo = .{};
    const err = ra8_c6link_wifi_ap_info(self.link, &ap);
    if (err != core.err_ok) return err;

    implementation.copyAp(sink, &ap);
    return core.err_ok;
}

fn opIdle(ctx: ?*anyopaque, ms: u16) callconv(.c) void {
    const self: *implementation.Handle = @ptrCast(@alignCast(ctx orelse return));
    const delay = self.transport.delay_ms orelse return;
    delay(self.transport.ctx, ms);
}

/// `k_ra8_wifi_backend_c6link`: the vtable a board points `ra8_wifi_cfg_t` at.
/// Committed to rodata so the address stays valid for the program's life, the
/// way the C's file-scope `const` did.
pub export const k_ra8_wifi_backend_c6link: facade.Backend = .{
    .open = &opOpen,
    .close = &opClose,
    .radio_up = &opRadioUp,
    .radio_down = &opRadioDown,
    .join = &opJoin,
    .leave = &opLeave,
    .service = &opService,
    .get_mac = &opGetMac,
    .get_ap = &opGetAp,
    .idle = &opIdle,
};

pub export fn ra8_wifi_c6link_setup(
    self: ?*implementation.Handle,
    cfg: ?*const implementation.Cfg,
    out_wcfg: ?*facade.Config,
) callconv(.c) u16 {
    const handle = self orelse return nullPtr("self");
    const source = cfg orelse return nullPtr("cfg");
    const sink = out_wcfg orelse return nullPtr("out_wcfg");
    if (source.link == null) return nullPtr("cfg.link");
    if (implementation.arenaTooSmall(source.arena_bytes)) return err_invalid_size;

    implementation.applyCfg(handle, source);
    sink.backend = &k_ra8_wifi_backend_c6link;
    sink.backend_ctx = handle;
    return core.err_ok;
}
