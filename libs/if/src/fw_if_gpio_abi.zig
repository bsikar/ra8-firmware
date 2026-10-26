//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for `fw_if_gpio.h`: the `fw_gpio` facade exported to C.
//!
//! Everything chip-specific lives behind `fw_gpio_iface_t`, so what is left
//! here is small on purpose: reject a malformed request, refuse an unbound
//! handle, and call the binding. Pin numbers are a board fact, so the
//! membrane does not range-check them; the binding owns that and answers
//! `k_ra8_err_not_found` for a pin it does not carry. Direction is the one
//! thing the membrane refuses on its own account: a zeroed `fw_gpio_pin_cfg_t`
//! is the shape a caller gets by forgetting to fill one in, and answering it
//! would hide that mistake. The pull's `none` is a real answer and passes.

const std = @import("std");
const core = @import("internal/root.zig");

const Err = core.Err;

/// `fw_gpio_direction_t`, `uint8_t`-backed as the header declares it.
pub const Direction = struct {
    pub const none: u8 = 0;
    pub const input: u8 = 1;
    pub const output: u8 = 2;
    pub const count: u8 = 3;
};

/// `fw_gpio_pull_t`, `uint8_t`-backed as the header declares it.
pub const Pull = struct {
    pub const none: u8 = 0;
    pub const up: u8 = 1;
    pub const down: u8 = 2;
    pub const count: u8 = 3;
};

/// `fw_gpio_pin_cfg_t`: direction and pull for one pin.
pub const PinCfg = extern struct {
    direction: u8,
    pull: u8,
};

/// `fw_gpio_iface_t`: the narrow ops struct a chip-and-board binding fills.
pub const Iface = extern struct {
    configure: ?*const fn (?*anyopaque, u16, PinCfg) callconv(.c) Err,
    write: ?*const fn (?*anyopaque, u16, bool) callconv(.c) Err,
    read: ?*const fn (?*anyopaque, u16, ?*bool) callconv(.c) Err,
};

/// `fw_gpio_t`: the caller-owned binding handle.
pub const Gpio = extern struct {
    iface: ?*const Iface,
    ctx: ?*anyopaque,
    bound: bool,
};

// The header pins both enumerations and the handle's flag to plain C uint8_t
// and C bool; both are load-bearing for the layouts above.
comptime {
    std.debug.assert(@sizeOf(PinCfg) == 2);
    std.debug.assert(@sizeOf(bool) == 1);
}

/// Whether a configuration names a direction this contract enumerates and a
/// pull it accepts. `k_fw_gpio_direction_none` is rejected along with anything
/// past the last enumerator; the pull's `none` is a real answer.
fn cfgValid(cfg: PinCfg) bool {
    const direction_ok = cfg.direction != Direction.none and cfg.direction < Direction.count;
    const pull_ok = cfg.pull < Pull.count;
    return direction_ok and pull_ok;
}

pub export fn fw_gpio_bind(gpio: ?*Gpio, iface: ?*const Iface, ctx: ?*anyopaque) callconv(.c) Err {
    const handle = gpio orelse return core.err_invalid_arg;
    const ops = iface orelse return core.err_invalid_arg;
    // A NULL op is a malformed binding, not a declined capability: a binding
    // that cannot read says so from read, not by leaving it unset.
    if (ops.configure == null or ops.write == null or ops.read == null) {
        return core.err_invalid_arg;
    }
    handle.iface = ops;
    handle.ctx = ctx;
    handle.bound = true;
    return core.ok;
}

pub export fn fw_gpio_configure(gpio: ?*const Gpio, pin: u16, cfg: PinCfg) callconv(.c) Err {
    // A zeroed cfg is a forgotten fill-in, not a configuration.
    if (!cfgValid(cfg)) return core.err_invalid_arg;

    const guard = check(gpio);
    if (guard != core.ok) return guard;

    const handle = gpio.?;
    return handle.iface.?.configure.?(handle.ctx, pin, cfg);
}

pub export fn fw_gpio_write(gpio: ?*const Gpio, pin: u16, value: bool) callconv(.c) Err {
    const guard = check(gpio);
    if (guard != core.ok) return guard;

    const handle = gpio.?;
    return handle.iface.?.write.?(handle.ctx, pin, value);
}

pub export fn fw_gpio_read(gpio: ?*const Gpio, pin: u16, out_value: ?*bool) callconv(.c) Err {
    const out = out_value orelse return core.err_invalid_arg;
    out.* = false;

    const guard = check(gpio);
    if (guard != core.ok) return guard;

    var value = false;
    const handle = gpio.?;
    const err = handle.iface.?.read.?(handle.ctx, pin, &value);
    if (err != core.ok) return err;
    out.* = value;
    return core.ok;
}

/// Common entry guard: bound handle.
fn check(gpio: ?*const Gpio) Err {
    const handle = gpio orelse return core.err_invalid_arg;
    if (!handle.bound) return core.err_not_initialized;
    return core.ok;
}
