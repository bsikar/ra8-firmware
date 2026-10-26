//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for `fw_if_spi.h`: the `fw_spi` facade exported to C.
//!
//! Everything chip-specific lives behind `fw_spi_iface_t`, so what is left
//! here is small on purpose: reject a malformed request, refuse an unbound
//! handle, and call the binding. Device indexes are a board fact, so the
//! membrane does not range-check them; the binding owns that and answers
//! `k_ra8_err_not_found` for one it does not carry. The line description is
//! the one thing the membrane refuses on its own account: a zero clock is not
//! a clock, and a zeroed `fw_spi_config_t` is the shape a caller gets by
//! forgetting to fill one in.

const std = @import("std");
const core = @import("internal/root.zig");

const Err = core.Err;

/// `fw_spi_mode_t`, `uint8_t`-backed as the header declares it.
pub const Mode = struct {
    pub const none: u8 = 0;
    pub const mode_0: u8 = 1;
    pub const mode_1: u8 = 2;
    pub const mode_2: u8 = 3;
    pub const mode_3: u8 = 4;
    pub const count: u8 = 5;
};

/// `fw_spi_config_t`: clock ceiling and clock phase mode.
pub const Config = extern struct {
    clock_hz: u32,
    mode: u8,
};

/// `fw_spi_iface_t`: the narrow ops struct a chip-and-board binding fills.
pub const Iface = extern struct {
    configure: ?*const fn (?*anyopaque, u8, Config) callconv(.c) Err,
    write: ?*const fn (?*anyopaque, u8, ?[*]const u8, usize) callconv(.c) Err,
    exchange: ?*const fn (?*anyopaque, u8, ?[*]const u8, ?[*]u8, usize) callconv(.c) Err,
};

/// `fw_spi_t`: the caller-owned binding handle.
pub const Spi = extern struct {
    iface: ?*const Iface,
    ctx: ?*anyopaque,
    bound: bool,
};

// The header pins the config's numeric width and the handle's flag to plain C
// uint32_t/uint8_t and C bool; both are load-bearing for the layouts above.
comptime {
    std.debug.assert(@sizeOf(Config) == 8);
    std.debug.assert(@sizeOf(bool) == 1);
}

/// Whether a line description names fields this contract enumerates. A zero
/// clock is refused along with anything past the last mode enumerator.
fn cfgValid(cfg: Config) bool {
    return cfg.clock_hz != 0 and cfg.mode != Mode.none and cfg.mode < Mode.count;
}

/// Common entry guard: bound handle.
fn check(dev: ?*const Spi) Err {
    const handle = dev orelse return core.err_invalid_arg;
    if (!handle.bound) return core.err_not_initialized;
    return core.ok;
}

pub export fn fw_spi_bind(dev: ?*Spi, iface: ?*const Iface, ctx: ?*anyopaque) callconv(.c) Err {
    const handle = dev orelse return core.err_invalid_arg;
    const ops = iface orelse return core.err_invalid_arg;
    // A NULL op is a malformed binding, not a declined capability: a binding
    // that cannot exchange says so from exchange, not by leaving it unset.
    if (ops.configure == null or ops.write == null or ops.exchange == null) {
        return core.err_invalid_arg;
    }
    handle.iface = ops;
    handle.ctx = ctx;
    handle.bound = true;
    return core.ok;
}

pub export fn fw_spi_configure(dev: ?*const Spi, dev_index: u8, cfg: Config) callconv(.c) Err {
    if (!cfgValid(cfg)) return core.err_invalid_arg;

    const guard = check(dev);
    if (guard != core.ok) return guard;

    const handle = dev.?;
    return handle.iface.?.configure.?(handle.ctx, dev_index, cfg);
}

pub export fn fw_spi_write(dev: ?*const Spi, dev_index: u8, bytes: ?[*]const u8, len: usize) callconv(.c) Err {
    // A zero-byte write asks for nothing; it is satisfied without waking the
    // peripheral, and `bytes` may be NULL because nothing is read.
    if (len == 0) return core.ok;
    const buffer = bytes orelse return core.err_invalid_arg;

    const guard = check(dev);
    if (guard != core.ok) return guard;

    const handle = dev.?;
    return handle.iface.?.write.?(handle.ctx, dev_index, buffer, len);
}

pub export fn fw_spi_exchange(dev: ?*const Spi, dev_index: u8, tx: ?[*]const u8, rx: ?[*]u8, len: usize) callconv(.c) Err {
    // A zero-byte exchange asks for nothing; it is satisfied without waking
    // the peripheral.
    if (len == 0) return core.ok;
    // Both directions are the same call; a missing side is a malformed
    // request, not a mode.
    if (tx == null or rx == null) return core.err_invalid_arg;

    const guard = check(dev);
    if (guard != core.ok) return guard;

    const handle = dev.?;
    return handle.iface.?.exchange.?(handle.ctx, dev_index, tx.?, rx.?, len);
}
