//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for `fw_if_i2c.h`: the `fw_i2c` facade exported to C.
//!
//! Everything chip-specific lives behind `fw_i2c_iface_t`, so what is left
//! here is small on purpose: reject a malformed request, refuse an unbound
//! handle, and call the binding. Bus indexes are a board fact, so the
//! membrane does not range-check them; the binding owns that and answers
//! `k_ra8_err_not_found` for one it does not carry. Two things are the
//! membrane's own account: a target address wider than 7 bits carries an
//! R/W bit the caller was not asked to set, and a zeroed `fw_i2c_config_t`
//! is the shape a caller gets by forgetting to fill one in.

const std = @import("std");
const core = @import("internal/root.zig");

const Err = core.Err;

/// `fw_i2c_speed_t`, `uint8_t`-backed as the header declares it.
pub const Speed = struct {
    pub const none: u8 = 0;
    pub const std: u8 = 1;
    pub const fast: u8 = 2;
    pub const fast_plus: u8 = 3;
    pub const count: u8 = 4;
};

/// `fw_i2c_config_t`: the bus speed.
pub const Config = extern struct {
    speed: u8,
};

/// `fw_i2c_iface_t`: the narrow ops struct a chip-and-board binding fills.
pub const Iface = extern struct {
    configure: ?*const fn (?*anyopaque, u8, Config) callconv(.c) Err,
    write: ?*const fn (?*anyopaque, u8, u8, ?[*]const u8, usize) callconv(.c) Err,
    read: ?*const fn (?*anyopaque, u8, u8, ?[*]u8, usize) callconv(.c) Err,
};

/// `fw_i2c_t`: the caller-owned binding handle.
pub const I2c = extern struct {
    iface: ?*const Iface,
    ctx: ?*anyopaque,
    bound: bool,
};

// The header pins the config's numeric width and the handle's flag to plain C
// uint8_t and C bool; both are load-bearing for the layouts above.
comptime {
    std.debug.assert(@sizeOf(Config) == 1);
    std.debug.assert(@sizeOf(bool) == 1);
}

/// Whether a speed description names a speed this contract enumerates. The
/// zeroed shape is the unconfigured config, refused like any out-of-range
/// value.
fn cfgValid(cfg: Config) bool {
    return cfg.speed != Speed.none and cfg.speed < Speed.count;
}

/// Whether an address is the 7-bit form the datasheets print, no R/W bit.
fn addrValid(target: u8) bool {
    return target <= 0x7F;
}

/// Common entry guard: bound handle.
fn check(bus: ?*const I2c) Err {
    const handle = bus orelse return core.err_invalid_arg;
    if (!handle.bound) return core.err_not_initialized;
    return core.ok;
}

pub export fn fw_i2c_bind(bus: ?*I2c, iface: ?*const Iface, ctx: ?*anyopaque) callconv(.c) Err {
    const handle = bus orelse return core.err_invalid_arg;
    const ops = iface orelse return core.err_invalid_arg;
    // A NULL op is a malformed binding, not a declined capability: a binding
    // that cannot receive says so from read, not by leaving it unset.
    if (ops.configure == null or ops.write == null or ops.read == null) {
        return core.err_invalid_arg;
    }
    handle.iface = ops;
    handle.ctx = ctx;
    handle.bound = true;
    return core.ok;
}

pub export fn fw_i2c_configure(bus: ?*const I2c, bus_index: u8, cfg: Config) callconv(.c) Err {
    if (!cfgValid(cfg)) return core.err_invalid_arg;

    const guard = check(bus);
    if (guard != core.ok) return guard;

    const handle = bus.?;
    return handle.iface.?.configure.?(handle.ctx, bus_index, cfg);
}

pub export fn fw_i2c_write(bus: ?*const I2c, bus_index: u8, target: u8, bytes: ?[*]const u8, len: usize) callconv(.c) Err {
    // A zero-byte write asks for nothing; it is satisfied without waking the
    // bus, and `bytes` may be NULL because nothing is read.
    if (len == 0) return core.ok;
    if (!addrValid(target)) return core.err_invalid_arg;
    const buffer = bytes orelse return core.err_invalid_arg;

    const guard = check(bus);
    if (guard != core.ok) return guard;

    const handle = bus.?;
    return handle.iface.?.write.?(handle.ctx, bus_index, target, buffer, len);
}

pub export fn fw_i2c_read(bus: ?*const I2c, bus_index: u8, target: u8, out: ?[*]u8, len: usize) callconv(.c) Err {
    // A zero-byte read asks for nothing; it is satisfied without waking the
    // bus.
    if (len == 0) return core.ok;
    if (!addrValid(target)) return core.err_invalid_arg;
    const buffer = out orelse return core.err_invalid_arg;

    const guard = check(bus);
    if (guard != core.ok) return guard;

    const handle = bus.?;
    return handle.iface.?.read.?(handle.ctx, bus_index, target, buffer, len);
}
