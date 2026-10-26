//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for `fw_if_rng.h`: the `fw_rng` facade exported to C.
//!
//! Everything chip-specific lives behind `fw_rng_iface_t`, so what is left
//! here is small on purpose: reject a malformed request, refuse an unbound
//! handle, and call the binding. The one thing this membrane cannot verify is
//! the fill contract itself (fresh bytes, buffer untouched on error); the
//! header states it as a contract, and the tests pin that the membrane does
//! not write to the buffer on its own account.

const std = @import("std");
const core = @import("internal/root.zig");

const Err = core.Err;

/// `fw_rng_iface_t`: the two ops a chip binding fills.
pub const Iface = extern struct {
    fill: ?*const fn (?*anyopaque, ?[*]u8, usize) callconv(.c) Err,
    ready: ?*const fn (?*anyopaque, ?*bool) callconv(.c) Err,
};

/// `fw_rng_t`: the caller-owned binding handle.
pub const Rng = extern struct {
    iface: ?*const Iface,
    ctx: ?*anyopaque,
    bound: bool,
};

// The header pins the handle to a plain C bool; it is load-bearing for the
// layout above.
comptime {
    std.debug.assert(@sizeOf(bool) == 1);
}

/// Common entry guard: bound handle.
fn check(rng: ?*const Rng) Err {
    const handle = rng orelse return core.err_invalid_arg;
    if (!handle.bound) return core.err_not_initialized;
    return core.ok;
}

pub export fn fw_rng_bind(rng: ?*Rng, iface: ?*const Iface, ctx: ?*anyopaque) callconv(.c) Err {
    const handle = rng orelse return core.err_invalid_arg;
    const ops = iface orelse return core.err_invalid_arg;
    // A NULL op is a malformed binding, not a declined capability: a binding
    // that always serves still answers ready, so both entries are required.
    if (ops.fill == null or ops.ready == null) {
        return core.err_invalid_arg;
    }
    handle.iface = ops;
    handle.ctx = ctx;
    handle.bound = true;
    return core.ok;
}

pub export fn fw_rng_bytes(rng: ?*const Rng, out: ?[*]u8, len: usize) callconv(.c) Err {
    // A zero-byte fill asks for nothing; it is satisfied without waking the
    // entropy source, and `out` may be NULL because nothing is written.
    if (len == 0) return core.ok;
    const buffer = out orelse return core.err_invalid_arg;

    const guard = check(rng);
    if (guard != core.ok) return guard;

    const handle = rng.?;
    return handle.iface.?.fill.?(handle.ctx, buffer, len);
}

pub export fn fw_rng_ready(rng: ?*const Rng, out_ready: ?*bool) callconv(.c) Err {
    const out = out_ready orelse return core.err_invalid_arg;
    out.* = false;

    const guard = check(rng);
    if (guard != core.ok) return guard;

    var ready = false;
    const handle = rng.?;
    const err = handle.iface.?.ready.?(handle.ctx, &ready);
    if (err != core.ok) return err;
    out.* = ready;
    return core.ok;
}
