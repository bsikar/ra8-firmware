//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The three `fw_if_gpio` binding ops, over the stub profile.
//!
//! The port facade has already rejected a null output and an unbound handle
//! before control reaches here, so these check only what the profile itself
//! can still refuse: a pin the board does not carry, a malformed config, a
//! write to a pin that is not an output.
//!
//! Static storage with no context, because the stub serves one port: the ops
//! ignore the context the port hands them.

const stub = @import("gpio_stub");

/// `ra8_err.h` codes these ops can return.
pub const Err = struct {
    pub const ok: u16 = 0;
    pub const invalid_arg: u16 = 0x103;
    pub const invalid_state: u16 = 0x104;
    pub const not_found: u16 = 0x106;
};

extern fn fw_gpio_bind(gpio: ?*Handle, iface: ?*const Iface, ctx: ?*anyopaque) callconv(.c) u16;

/// `fw_gpio_pin_cfg_t`: direction and pull for one pin, as the C header
/// lays them out (two bytes, uint8_t each).
pub const PinCfg = extern struct {
    direction: u8,
    pull: u8,
};

/// `fw_gpio_iface_t`. All three ops are required by the facade's bind, so no
/// pointer is optional here.
pub const Iface = extern struct {
    configure: *const fn (ctx: ?*anyopaque, pin: u16, cfg: PinCfg) callconv(.c) u16,
    write: *const fn (ctx: ?*anyopaque, pin: u16, value: bool) callconv(.c) u16,
    read: *const fn (ctx: ?*anyopaque, pin: u16, out_value: ?*bool) callconv(.c) u16,
};

/// `fw_gpio_t`, caller-owned and opaque once `fw_gpio_bind` has filled it.
pub const Handle = extern struct {
    iface: ?*const Iface,
    ctx: ?*anyopaque,
    bound: bool,
};

/// Put one pin into a direction and pull. A pin the board does not carry is
/// `not_found`, the same answer the port facade gives.
pub export fn fw_gpio_stub_configure(ctx: ?*anyopaque, pin: u16, cfg: PinCfg) callconv(.c) u16 {
    _ = ctx;

    stub.configure(pin, .{ .direction = cfg.direction, .pull = cfg.pull }) catch |err| {
        return switch (err) {
            error.InvalidArg => Err.invalid_arg,
            error.NotFound => Err.not_found,
        };
    };

    return Err.ok;
}

/// Drive one output pin. An input pin answers `invalid_state`, per the
/// port's contract that a write to an input is refused.
pub export fn fw_gpio_stub_write(ctx: ?*anyopaque, pin: u16, value: bool) callconv(.c) u16 {
    _ = ctx;

    stub.write(pin, value) catch |err| {
        return switch (err) {
            error.InvalidState => Err.invalid_state,
            error.NotFound => Err.not_found,
        };
    };

    return Err.ok;
}

/// Sample one pin's level: what an output drives, or the board's tied level
/// (low) for an input.
pub export fn fw_gpio_stub_read(ctx: ?*anyopaque, pin: u16, out_value: ?*bool) callconv(.c) u16 {
    _ = ctx;

    const out = out_value orelse return Err.invalid_arg;

    const level = stub.read(pin) catch {
        return Err.not_found;
    };

    out.* = level;
    return Err.ok;
}

/// Static storage with no context, because the stub profile is a singleton.
/// The pointer outlives any handle bound to it.
const iface: Iface = .{
    .configure = &fw_gpio_stub_configure,
    .write = &fw_gpio_stub_write,
    .read = &fw_gpio_stub_read,
};

pub export fn fw_gpio_stub_iface() callconv(.c) *const Iface {
    return &iface;
}

pub export fn fw_gpio_stub_bind(gpio: ?*Handle) callconv(.c) u16 {
    return fw_gpio_bind(gpio, &iface, null);
}
