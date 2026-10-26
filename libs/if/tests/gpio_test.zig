//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Vectors for the `fw_gpio` facade, driven through a fake binding that keeps
//! a small pin table. Everything proved here is facade behaviour no chip can
//! change: the entry guards, the malformed-binding rejection, the zeroed-cfg
//! refusal and the membrane's hands-off rule for `out_value` on error.

const std = @import("std");
const abi = @import("abi");

const Gpio = abi.Gpio;
const Iface = abi.Iface;
const PinCfg = abi.PinCfg;

const ok: u16 = 0;
const err_invalid_arg: u16 = 0x103;
const err_not_found: u16 = 0x106;
const err_not_initialized: u16 = 0x10F;
const err_invalid_state: u16 = 0x104;

const input_no_pull = PinCfg{ .direction = abi.Direction.input, .pull = abi.Pull.none };
const output_no_pull = PinCfg{ .direction = abi.Direction.output, .pull = abi.Pull.none };

/// What the fake binding was asked, so a vector can assert on it.
const Fake = struct {
    last_pin: u16 = 0,
    last_cfg: PinCfg = .{ .direction = 0, .pull = 0 },
    last_written: bool = false,
    level: bool = true,
    configure_err: u16 = ok,
    write_err: u16 = ok,
    read_err: u16 = ok,
    calls: u32 = 0,
};

var fake: Fake = .{};

fn fakeConfigure(ctx: ?*anyopaque, pin: u16, cfg: PinCfg) callconv(.c) u16 {
    const state: *Fake = @ptrCast(@alignCast(ctx.?));
    state.calls += 1;
    state.last_pin = pin;
    state.last_cfg = cfg;
    return state.configure_err;
}

fn fakeWrite(ctx: ?*anyopaque, pin: u16, value: bool) callconv(.c) u16 {
    const state: *Fake = @ptrCast(@alignCast(ctx.?));
    state.calls += 1;
    state.last_pin = pin;
    state.last_written = value;
    return state.write_err;
}

fn fakeRead(ctx: ?*anyopaque, pin: u16, out_value: ?*bool) callconv(.c) u16 {
    const state: *Fake = @ptrCast(@alignCast(ctx.?));
    state.calls += 1;
    state.last_pin = pin;
    if (state.read_err != ok) return state.read_err;
    out_value.?.* = state.level;
    return ok;
}

const fake_ops = Iface{ .configure = fakeConfigure, .write = fakeWrite, .read = fakeRead };

fn bound() Gpio {
    fake = .{};
    var gpio = std.mem.zeroes(Gpio);
    std.debug.assert(abi.fw_gpio_bind(&gpio, &fake_ops, &fake) == ok);
    return gpio;
}

test "bind rejects a null handle, a null ops struct or an incomplete ops struct" {
    var gpio = std.mem.zeroes(Gpio);
    try std.testing.expectEqual(err_invalid_arg, abi.fw_gpio_bind(null, &fake_ops, null));
    try std.testing.expectEqual(err_invalid_arg, abi.fw_gpio_bind(&gpio, null, null));
    try std.testing.expect(!gpio.bound);

    const no_configure = Iface{ .configure = null, .write = fakeWrite, .read = fakeRead };
    const no_write = Iface{ .configure = fakeConfigure, .write = null, .read = fakeRead };
    const no_read = Iface{ .configure = fakeConfigure, .write = fakeWrite, .read = null };
    try std.testing.expectEqual(err_invalid_arg, abi.fw_gpio_bind(&gpio, &no_configure, null));
    try std.testing.expectEqual(err_invalid_arg, abi.fw_gpio_bind(&gpio, &no_write, null));
    try std.testing.expectEqual(err_invalid_arg, abi.fw_gpio_bind(&gpio, &no_read, null));
    try std.testing.expect(!gpio.bound);
}

test "an unbound handle refuses configure, write and read" {
    const gpio = std.mem.zeroes(Gpio);
    try std.testing.expectEqual(err_not_initialized, abi.fw_gpio_configure(&gpio, 0, input_no_pull));
    try std.testing.expectEqual(err_not_initialized, abi.fw_gpio_write(&gpio, 0, true));
    var level = true;
    try std.testing.expectEqual(err_not_initialized, abi.fw_gpio_read(&gpio, 0, &level));
    try std.testing.expect(!level);
}

test "configure refuses a zeroed cfg and a direction past the enumeration" {
    var gpio = bound();
    try std.testing.expectEqual(err_invalid_arg, abi.fw_gpio_configure(&gpio, 0, PinCfg{ .direction = 0, .pull = 0 }));
    const bad_direction = PinCfg{ .direction = 9, .pull = abi.Pull.none };
    try std.testing.expectEqual(err_invalid_arg, abi.fw_gpio_configure(&gpio, 0, bad_direction));
    // The pull's none is a real answer, and a pull past its enumeration is not.
    try std.testing.expectEqual(ok, abi.fw_gpio_configure(&gpio, 0, input_no_pull));
    const bad_pull = PinCfg{ .direction = abi.Direction.input, .pull = 7 };
    try std.testing.expectEqual(err_invalid_arg, abi.fw_gpio_configure(&gpio, 0, bad_pull));
    try std.testing.expectEqual(@as(u32, 1), fake.calls);
}

test "configure carries the pin and the cfg through to the binding" {
    var gpio = bound();
    const cfg = PinCfg{ .direction = abi.Direction.input, .pull = abi.Pull.up };
    try std.testing.expectEqual(ok, abi.fw_gpio_configure(&gpio, 3, cfg));
    try std.testing.expectEqual(@as(u16, 3), fake.last_pin);
    try std.testing.expectEqual(@as(u8, abi.Direction.input), fake.last_cfg.direction);
    try std.testing.expectEqual(@as(u8, abi.Pull.up), fake.last_cfg.pull);

    fake.configure_err = err_not_found;
    try std.testing.expectEqual(err_not_found, abi.fw_gpio_configure(&gpio, 99, cfg));
}

test "write carries the pin and the level through to the binding" {
    var gpio = bound();
    try std.testing.expectEqual(ok, abi.fw_gpio_write(&gpio, 5, true));
    try std.testing.expectEqual(@as(u16, 5), fake.last_pin);
    try std.testing.expect(fake.last_written);

    try std.testing.expectEqual(ok, abi.fw_gpio_write(&gpio, 5, false));
    try std.testing.expect(!fake.last_written);

    fake.write_err = err_invalid_state;
    try std.testing.expectEqual(err_invalid_state, abi.fw_gpio_write(&gpio, 5, true));
}

test "read reports the binding's answer and clears out_value first" {
    var gpio = bound();
    var level = false;
    try std.testing.expectEqual(ok, abi.fw_gpio_read(&gpio, 2, &level));
    try std.testing.expect(level);
    try std.testing.expectEqual(@as(u16, 2), fake.last_pin);

    fake.level = false;
    try std.testing.expectEqual(ok, abi.fw_gpio_read(&gpio, 2, &level));
    try std.testing.expect(!level);

    fake.read_err = err_not_found;
    level = true;
    try std.testing.expectEqual(err_not_found, abi.fw_gpio_read(&gpio, 99, &level));
    try std.testing.expect(!level);

    try std.testing.expectEqual(err_invalid_arg, abi.fw_gpio_read(&gpio, 2, null));
}
