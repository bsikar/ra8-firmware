//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The two `fw_if_rng` binding ops, over the stub profile.
//!
//! The port facade has already rejected a null output and an unbound handle
//! before control reaches here, so these check only what the profile itself
//! can still refuse. The one thing no membrane can verify is the fill
//! contract itself (fresh bytes, buffer untouched on error); the header
//! states it as a contract, and this stub honors it trivially because it has
//! no error path in `fill`.
//!
//! Static storage with no context, because the stub serves one source: the
//! ops ignore the context the port hands them.

const stub = @import("rng_stub");

/// `ra8_err.h` codes these ops can return.
pub const Err = struct {
    pub const ok: u16 = 0;
    pub const invalid_arg: u16 = 0x103;
};

extern fn fw_rng_bind(rng: ?*Handle, iface: ?*const Iface, ctx: ?*anyopaque) callconv(.c) u16;

/// `fw_rng_iface_t`. Both ops are required by the facade's bind, so neither
/// pointer is optional here.
pub const Iface = extern struct {
    fill: *const fn (ctx: ?*anyopaque, out: ?[*]u8, len: usize) callconv(.c) u16,
    ready: *const fn (ctx: ?*anyopaque, out_ready: ?*bool) callconv(.c) u16,
};

/// `fw_rng_t`, caller-owned and opaque once `fw_rng_bind` has filled it.
pub const Handle = extern struct {
    iface: ?*const Iface,
    ctx: ?*anyopaque,
    bound: bool,
};

/// Fill a buffer with the stub pattern. A zero-length request is satisfied
/// without waking the source, matching the facade's own contract.
pub export fn fw_rng_stub_fill(ctx: ?*anyopaque, out: ?[*]u8, len: usize) callconv(.c) u16 {
    _ = ctx;

    if (len == 0) return Err.ok;

    const buffer = out orelse return Err.invalid_arg;
    stub.fill(buffer[0..len]);
    return Err.ok;
}

/// Report whether the source can serve right now. The stub always can.
pub export fn fw_rng_stub_ready(ctx: ?*anyopaque, out_ready: ?*bool) callconv(.c) u16 {
    _ = ctx;

    const out = out_ready orelse return Err.invalid_arg;
    out.* = stub.ready();
    return Err.ok;
}

/// Static storage with no context, because the stub profile is a singleton.
/// The pointer outlives any handle bound to it.
const iface: Iface = .{
    .fill = &fw_rng_stub_fill,
    .ready = &fw_rng_stub_ready,
};

pub export fn fw_rng_stub_iface() callconv(.c) *const Iface {
    return &iface;
}

pub export fn fw_rng_stub_bind(rng: ?*Handle) callconv(.c) u16 {
    return fw_rng_bind(rng, &iface, null);
}
