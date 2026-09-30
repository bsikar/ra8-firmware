//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for `ra8_ov5640_bind_i2c`: the one adapter that used to be
//! written out in every consuming app, translating between the part's SCCB
//! transport interface (`ra8_ov5640_bus_t`, 16-bit register pointers) and the
//! house I2C seam `ra8_i2c_bus_ops_t`.
//!
//! Kept out of `ra8_ov5640_abi.zig` deliberately, exactly as the C kept it out
//! of `ra8_ov5640.c`: that membrane is the pure register-level driver and names
//! no bus abstraction. This file is the one place that does, so the split stays
//! readable in the link map. Same shape as `ra8_lsm6dso_bind.c`, the sibling
//! binder.

const std = @import("std");
const driver = @import("ra8_ov5640_abi.zig");
const framing = @import("internal/i2c_bind.zig");

/// Subset of `ra8_err_t` this binder returns on its own behalf. Transport
/// codes pass through from the bound seam untouched.
const BindError = enum(u16) {
    ok = 0,
    null_ptr = 0x504,
};

const ok: u16 = @intFromEnum(BindError.ok);
const null_ptr: u16 = @intFromEnum(BindError.null_ptr);

/// Component tag on this file's log lines, matching the C's call sites.
const tag: [*:0]const u8 = "ov5640_bind";

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;

/// `ra8_i2c_bus_ops_t.write`.
pub const WriteFn = *const fn (
    ctx: ?*anyopaque,
    addr: u8,
    data: ?[*]const u8,
    len: u32,
    send_stop: bool,
) callconv(.c) u16;

/// `ra8_i2c_bus_ops_t.read`.
pub const ReadFn = *const fn (ctx: ?*anyopaque, addr: u8, data: ?[*]u8, len: u32) callconv(.c) u16;

/// `ra8_i2c_bus_ops_t.transfer`.
pub const TransferFn = *const fn (
    ctx: ?*anyopaque,
    addr: u8,
    wr: ?[*]const u8,
    wr_len: u32,
    rd: ?[*]u8,
    rd_len: u32,
) callconv(.c) u16;

/// `ra8_i2c_bus_ops_t`: the house I2C seam, in header order.
pub const BusOps = extern struct {
    write: ?WriteFn = null,
    read: ?ReadFn = null,
    transfer: ?TransferFn = null,
    ctx: ?*anyopaque = null,
};

/// `ra8_ov5640_i2c_ctx_t`: caller-owned binding state, holding the seam by
/// value. It is what the sensor's `ctx` cookie points at once bound, so it
/// must out-live the device.
pub const I2cCtx = extern struct {
    bus: BusOps = .{},
};

comptime {
    const word = @sizeOf(usize);
    std.debug.assert(@sizeOf(BusOps) == word * 4);
    std.debug.assert(@offsetOf(BusOps, "write") == 0);
    std.debug.assert(@offsetOf(BusOps, "read") == word);
    std.debug.assert(@offsetOf(BusOps, "transfer") == word * 2);
    std.debug.assert(@offsetOf(BusOps, "ctx") == word * 3);
    std.debug.assert(@sizeOf(I2cCtx) == @sizeOf(BusOps));
    std.debug.assert(@offsetOf(I2cCtx, "bus") == 0);
}

/// Reject a `nullptr` argument with the tag and message the C emitted.
fn rejectNull(pointer: ?*const anyopaque, message: [*:0]const u8) bool {
    if (pointer == null) {
        ra8_log_emit_error(tag, message);
        return true;
    }
    return false;
}

/// Read one register through the bound house seam: one write-RESTART-read of
/// the two-byte register pointer, which is exactly the seam's `transfer`
/// shape, so this adds no framing of its own beyond packing the pointer.
fn readTrampoline(ctx: ?*anyopaque, address: u8, reg: u16, out_value: ?*u8) callconv(.c) u16 {
    if (rejectNull(ctx, "i2c_read: ctx")) return null_ptr;
    if (rejectNull(out_value, "i2c_read: out_value")) return null_ptr;
    const binding: *const I2cCtx = @ptrCast(@alignCast(ctx.?));
    const transfer = binding.bus.transfer orelse return null_ptr;

    var pointer = framing.packReg(reg);
    return transfer(
        binding.bus.ctx,
        address,
        &pointer,
        @intCast(pointer.len),
        @ptrCast(out_value.?),
        1,
    );
}

/// Write one register through the bound house seam. The seam writes a whole
/// frame in one call, so `[reg_hi][reg_lo][value]` is staged contiguously and
/// sent as a single write terminated with STOP.
fn writeTrampoline(ctx: ?*anyopaque, address: u8, reg: u16, value: u8) callconv(.c) u16 {
    if (rejectNull(ctx, "i2c_write: ctx")) return null_ptr;
    const binding: *const I2cCtx = @ptrCast(@alignCast(ctx.?));
    const write = binding.bus.write orelse return null_ptr;

    var frame = framing.packWrite(reg, value);
    return write(binding.bus.ctx, address, &frame, @intCast(frame.len), true);
}

/// `ra8_ov5640_bind_i2c`: bind the sensor to the house I2C seam, delay
/// callback included. The delay stays a separate callback because the seam is
/// transfer-only by design and the sensor's reset and mode-switch waits are
/// real.
pub export fn ra8_ov5640_bind_i2c(
    out_dev: ?*driver.Device,
    out_ctx: ?*I2cCtx,
    ops: ?*const BusOps,
    delay_ms: ?driver.DelayFn,
) callconv(.c) u16 {
    if (rejectNull(out_dev, "bind_i2c: out_dev")) return null_ptr;
    if (rejectNull(out_ctx, "bind_i2c: out_ctx")) return null_ptr;
    if (rejectNull(ops, "bind_i2c: ops")) return null_ptr;
    if (rejectNull(@ptrCast(delay_ms), "bind_i2c: delay_ms")) return null_ptr;
    const seam = ops.?;
    if (rejectNull(@ptrCast(seam.write), "bind_i2c: ops->write")) return null_ptr;
    if (rejectNull(@ptrCast(seam.transfer), "bind_i2c: ops->transfer")) return null_ptr;

    const binding = out_ctx.?;
    binding.* = .{ .bus = seam.* };

    const bus = driver.Bus{
        .read_reg = readTrampoline,
        .write_reg = writeTrampoline,
        .delay_ms = delay_ms,
        .ctx = binding,
    };
    return driver.ra8_ov5640_init(out_dev.?, &bus);
}
