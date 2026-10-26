//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Vectors for the `fw_i2c` facade, driven through a fake binding that keeps
//! a transmit capture buffer and a fixed receive pattern. Everything proved
//! here is facade behaviour no chip can change: the entry guards, the
//! malformed binding and config rejections, the 7-bit address rule, the
//! zero-length no-ops, and the refusal paths.

const std = @import("std");
const abi = @import("abi");

const I2c = abi.I2c;
const Iface = abi.Iface;
const Config = abi.Config;

const ok: u16 = 0;
const err_invalid_arg: u16 = 0x103;
const err_not_found: u16 = 0x106;
const err_not_initialized: u16 = 0x10F;
const err_busy: u16 = 0x109;

const fast = Config{ .speed = abi.Speed.fast };

/// What the fake binding was asked, so a vector can assert on it.
const Fake = struct {
    last_bus: u8 = 0,
    last_target: u8 = 0,
    last_cfg: Config = .{ .speed = 0 },
    sent: [16]u8 = @splat(0),
    sent_len: usize = 0,
    configure_err: u16 = ok,
    write_err: u16 = ok,
    read_err: u16 = ok,
    calls: u32 = 0,
};

var fake: Fake = .{};

fn fakeConfigure(ctx: ?*anyopaque, bus: u8, cfg: Config) callconv(.c) u16 {
    const state: *Fake = @ptrCast(@alignCast(ctx.?));
    state.calls += 1;
    state.last_bus = bus;
    state.last_cfg = cfg;
    return state.configure_err;
}

fn fakeWrite(ctx: ?*anyopaque, bus: u8, target: u8, bytes: ?[*]const u8, len: usize) callconv(.c) u16 {
    const state: *Fake = @ptrCast(@alignCast(ctx.?));
    state.calls += 1;
    state.last_bus = bus;
    state.last_target = target;
    if (state.write_err != ok) return state.write_err;
    if (bytes) |buf| {
        for (0..len) |i| {
            if (i < state.sent.len) state.sent[i] = buf[i];
        }
        state.sent_len = @min(len, state.sent.len);
    }
    return ok;
}

fn fakeRead(ctx: ?*anyopaque, bus: u8, target: u8, out: ?[*]u8, len: usize) callconv(.c) u16 {
    const state: *Fake = @ptrCast(@alignCast(ctx.?));
    state.calls += 1;
    state.last_bus = bus;
    state.last_target = target;
    if (state.read_err != ok) return state.read_err;
    if (out) |buf| {
        for (0..len) |i| buf[i] = 0x5A;
    }
    return ok;
}

const fake_ops = Iface{ .configure = fakeConfigure, .write = fakeWrite, .read = fakeRead };

fn bound() I2c {
    fake = .{};
    var bus = std.mem.zeroes(I2c);
    std.debug.assert(abi.fw_i2c_bind(&bus, &fake_ops, &fake) == ok);
    return bus;
}

test "bind rejects a null handle, a null ops struct or an incomplete ops struct" {
    var bus = std.mem.zeroes(I2c);
    try std.testing.expectEqual(err_invalid_arg, abi.fw_i2c_bind(null, &fake_ops, null));
    try std.testing.expectEqual(err_invalid_arg, abi.fw_i2c_bind(&bus, null, null));
    try std.testing.expect(!bus.bound);

    const no_configure = Iface{ .configure = null, .write = fakeWrite, .read = fakeRead };
    const no_write = Iface{ .configure = fakeConfigure, .write = null, .read = fakeRead };
    const no_read = Iface{ .configure = fakeConfigure, .write = fakeWrite, .read = null };
    try std.testing.expectEqual(err_invalid_arg, abi.fw_i2c_bind(&bus, &no_configure, null));
    try std.testing.expectEqual(err_invalid_arg, abi.fw_i2c_bind(&bus, &no_write, null));
    try std.testing.expectEqual(err_invalid_arg, abi.fw_i2c_bind(&bus, &no_read, null));
    try std.testing.expect(!bus.bound);
}

test "an unbound handle refuses configure, write and read" {
    const bus = std.mem.zeroes(I2c);
    try std.testing.expectEqual(err_not_initialized, abi.fw_i2c_configure(&bus, 0, fast));
    var byte: u8 = 0;
    try std.testing.expectEqual(
        err_not_initialized,
        abi.fw_i2c_write(&bus, 0, 0x50, @as([*]const u8, @ptrCast(&byte)), 1),
    );
    var rx: [4]u8 = @splat(0);
    try std.testing.expectEqual(
        err_not_initialized,
        abi.fw_i2c_read(&bus, 0, 0x50, &rx, 4),
    );
}

test "configure refuses a zeroed cfg and an out-of-range speed" {
    var bus = bound();
    try std.testing.expectEqual(err_invalid_arg, abi.fw_i2c_configure(&bus, 0, .{ .speed = 0 }));
    try std.testing.expectEqual(err_invalid_arg, abi.fw_i2c_configure(&bus, 0, .{ .speed = 9 }));
    try std.testing.expectEqual(@as(u32, 0), fake.calls);
}

test "configure carries the bus and the speed through to the binding" {
    var bus = bound();
    try std.testing.expectEqual(ok, abi.fw_i2c_configure(&bus, 1, fast));
    try std.testing.expectEqual(@as(u8, 1), fake.last_bus);
    try std.testing.expectEqual(@as(u8, abi.Speed.fast), fake.last_cfg.speed);

    fake.configure_err = err_not_found;
    try std.testing.expectEqual(err_not_found, abi.fw_i2c_configure(&bus, 9, fast));
}

test "an address wider than 7 bits is refused before the binding" {
    var bus = bound();
    const tx = [_]u8{0x01};
    var rx: [1]u8 = @splat(0);
    try std.testing.expectEqual(err_invalid_arg, abi.fw_i2c_write(&bus, 0, 0x80, &tx, 1));
    try std.testing.expectEqual(err_invalid_arg, abi.fw_i2c_write(&bus, 0, 0xFF, &tx, 1));
    try std.testing.expectEqual(err_invalid_arg, abi.fw_i2c_read(&bus, 0, 0x80, &rx, 1));
    try std.testing.expectEqual(@as(u32, 0), fake.calls);
}

test "a zero-byte write or read is a no-op that does not wake the bus" {
    var bus = bound();
    try std.testing.expectEqual(ok, abi.fw_i2c_write(&bus, 0, 0x50, null, 0));
    try std.testing.expectEqual(ok, abi.fw_i2c_write(&bus, 0, 0x50, undefined, 0));
    try std.testing.expectEqual(ok, abi.fw_i2c_read(&bus, 0, 0x50, null, 0));
    try std.testing.expectEqual(@as(u32, 0), fake.calls);
}

test "a null buffer with a nonzero length is refused before the binding" {
    var bus = bound();
    try std.testing.expectEqual(err_invalid_arg, abi.fw_i2c_write(&bus, 0, 0x50, null, 8));
    // The null receive buffer is the refusal itself; the binding is never
    // reached, so there is nothing to fill.
    try std.testing.expectEqual(err_invalid_arg, abi.fw_i2c_read(&bus, 0, 0x50, null, 8));
    try std.testing.expectEqual(@as(u32, 0), fake.calls);
}

test "write carries the target and bytes through to the binding" {
    var bus = bound();
    const reg = [_]u8{ 0x07, 0xD2 };
    try std.testing.expectEqual(ok, abi.fw_i2c_write(&bus, 0, 0x68, &reg, reg.len));
    try std.testing.expectEqual(@as(u32, 1), fake.calls);
    try std.testing.expectEqual(@as(u8, 0), fake.last_bus);
    try std.testing.expectEqual(@as(u8, 0x68), fake.last_target);
    try std.testing.expectEqual(@as(usize, 2), fake.sent_len);
    try std.testing.expectEqualSlices(u8, &reg, fake.sent[0..2]);

    fake.write_err = err_busy;
    try std.testing.expectEqual(err_busy, abi.fw_i2c_write(&bus, 0, 0x68, &reg, reg.len));
}

test "read carries the target through and fills the whole buffer" {
    var bus = bound();
    var rx: [3]u8 = @splat(0);
    try std.testing.expectEqual(ok, abi.fw_i2c_read(&bus, 1, 0x68, &rx, 3));
    try std.testing.expectEqual(@as(u32, 1), fake.calls);
    try std.testing.expectEqual(@as(u8, 1), fake.last_bus);
    try std.testing.expectEqual(@as(u8, 0x68), fake.last_target);
    try std.testing.expectEqualSlices(u8, &[_]u8{ 0x5A, 0x5A, 0x5A }, &rx);

    fake.read_err = err_busy;
    try std.testing.expectEqual(err_busy, abi.fw_i2c_read(&bus, 1, 0x68, &rx, 3));
}
