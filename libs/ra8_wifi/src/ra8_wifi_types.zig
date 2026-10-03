//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The published types the facade (`ra8_wifi_abi.zig`) and the ESP32-C6
//! backend (`ra8_wifi_c6link_abi.zig`) both name. This file exports nothing,
//! so the backend can import it and still be compiled as its own archive
//! member without re-emitting any of the facade's symbols.

const implementation = @import("internal/root.zig");

const Mac = implementation.Mac;
const Lease = implementation.Lease;
const Ap = implementation.Ap;

/// IP provider seam (`ra8_wifi_ip_bind_fn`).
pub const IpBindFn = *const fn (
    ip_ctx: ?*anyopaque,
    mac: ?*const Mac,
    out: ?*Lease,
) callconv(.c) u16;

/// The radio-operation vtable (`ra8_wifi_backend_t`). Every row is optional
/// here because the C struct holds plain function pointers a caller may leave
/// null, which is exactly what `ra8_wifi_init` rejects.
pub const Backend = extern struct {
    open: ?*const fn (ctx: ?*anyopaque) callconv(.c) u16 = null,
    close: ?*const fn (ctx: ?*anyopaque) callconv(.c) u16 = null,
    radio_up: ?*const fn (ctx: ?*anyopaque) callconv(.c) u16 = null,
    radio_down: ?*const fn (ctx: ?*anyopaque) callconv(.c) u16 = null,
    join: ?*const fn (
        ctx: ?*anyopaque,
        ssid: ?[*:0]const u8,
        psk: ?[*:0]const u8,
    ) callconv(.c) u16 = null,
    leave: ?*const fn (ctx: ?*anyopaque) callconv(.c) u16 = null,
    service: ?*const fn (ctx: ?*anyopaque, out_link: ?*u8) callconv(.c) u16 = null,
    get_mac: ?*const fn (ctx: ?*anyopaque, out: ?*Mac) callconv(.c) u16 = null,
    get_ap: ?*const fn (ctx: ?*anyopaque, out: ?*Ap) callconv(.c) u16 = null,
    idle: ?*const fn (ctx: ?*anyopaque, ms: u16) callconv(.c) void = null,
};

/// Selection a caller hands `ra8_wifi_init` (`ra8_wifi_cfg_t`).
pub const Config = extern struct {
    backend: ?*const Backend = null,
    backend_ctx: ?*anyopaque = null,
    ip_bind: ?IpBindFn = null,
    ip_ctx: ?*anyopaque = null,
};
