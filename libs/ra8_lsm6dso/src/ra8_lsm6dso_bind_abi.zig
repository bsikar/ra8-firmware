//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for `ra8_lsm6dso_bind_i2c`: the one adapter that used to
//! live in every consuming app, translating between the part's register-level
//! transport interface (`ra8_lsm6dso_bus_t`, kept because the part also runs
//! on SPI) and the house I2C seam `ra8_i2c_bus_ops_t`.
//!
//! Kept out of `ra8_lsm6dso_abi.zig` deliberately, exactly as the C kept it
//! out of `ra8_lsm6dso.c`: that membrane is the pure register-level driver and
//! names no bus abstraction. This file is the one place that does, so the
//! split stays readable in the link map. Sibling of the OV5640 binder.

const std = @import("std");
const driver = @import("ra8_lsm6dso_abi.zig");
const framing = @import("internal/i2c_bind.zig");

/// Subset of `ra8_err_t` this binder returns on its own behalf. Transport
/// codes pass through from the bound seam untouched.
const BindError = enum(u16) {
    ok = 0,
    invalid_arg = 0x103,
    null_ptr = 0x504,
};

const ok: u16 = @backingInt(BindError.ok);
const invalid_arg: u16 = @backingInt(BindError.invalid_arg);
const null_ptr: u16 = @backingInt(BindError.null_ptr);

/// Component tag on this file's log lines, matching the C `s_lsm6dso_bind_tag`.
const tag: [*:0]const u8 = "lsm6dso_bind";

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

/// `ra8_lsm6dso_i2c_ctx_t`: caller-owned binding state, holding the seam by
/// value plus the target address. It is what the driver's `ctx` cookie points
/// at once bound, so it must out-live the device.
pub const I2cCtx = extern struct {
    bus: BusOps = .{},
    target_7b: u8 = 0,
};

comptime {
    const word = @sizeOf(usize);
    std.debug.assert(@sizeOf(BusOps) == word * 4);
    std.debug.assert(@offsetOf(BusOps, "write") == 0);
    std.debug.assert(@offsetOf(BusOps, "read") == word);
    std.debug.assert(@offsetOf(BusOps, "transfer") == word * 2);
    std.debug.assert(@offsetOf(BusOps, "ctx") == word * 3);
    // The address byte takes one trailing pointer-sized slot after the seam,
    // on the host and on 32-bit Arm alike.
    std.debug.assert(@sizeOf(I2cCtx) == word * 5);
    std.debug.assert(@offsetOf(I2cCtx, "bus") == 0);
    std.debug.assert(@offsetOf(I2cCtx, "target_7b") == word * 4);
}

/// Reject a `nullptr` argument with the tag and message the C emitted.
fn rejectNull(pointer: ?*const anyopaque, message: [*:0]const u8) bool {
    if (pointer == null) {
        ra8_log_emit_error(tag, message);
        return true;
    }
    return false;
}

/// Read `len` bytes from `reg` through the bound house seam: one
/// write-RESTART-read, which is exactly the seam's `transfer` shape, so this
/// is a forward with no framing of its own. The part auto-increments
/// (DS12140 sec 6.1.2).
fn readTrampoline(ctx: ?*anyopaque, reg: u8, buf: ?[*]u8, len: u32) callconv(.c) u16 {
    if (rejectNull(ctx, "i2c_read: ctx")) return null_ptr;
    const binding: *const I2cCtx = @ptrCast(@alignCast(ctx.?));
    const transfer = binding.bus.transfer orelse return null_ptr;

    var pointer = reg;
    return transfer(binding.bus.ctx, binding.target_7b, @ptrCast(&pointer), 1, buf, len);
}

/// Write `len` bytes starting at `reg` through the bound seam, staged as one
/// `[reg][payload]` frame terminated with STOP.
fn writeTrampoline(ctx: ?*anyopaque, reg: u8, buf: ?[*]const u8, len: u32) callconv(.c) u16 {
    if (rejectNull(ctx, "i2c_write: ctx")) return null_ptr;
    if (!framing.payloadFits(len)) return invalid_arg;
    if (len > 0 and buf == null) return null_ptr;

    const payload = if (len > 0) buf.?[0..len] else &[_]u8{};
    const frame = framing.stageWrite(reg, payload);

    const binding: *const I2cCtx = @ptrCast(@alignCast(ctx.?));
    const write = binding.bus.write orelse return null_ptr;
    return write(
        binding.bus.ctx,
        binding.target_7b,
        frame.slice().ptr,
        @intCast(frame.len),
        true,
    );
}

/// `ra8_lsm6dso_bind_i2c`: fill the binding state from the house seam, build
/// the driver's register-level bus over it, and initialise in one call. A
/// refused init leaves no half-bound state behind.
pub export fn ra8_lsm6dso_bind_i2c(
    out_dev: ?*driver.Device,
    out_ctx: ?*I2cCtx,
    ops: ?*const BusOps,
    target_7b: u8,
) callconv(.c) u16 {
    if (rejectNull(out_dev, "bind_i2c: out_dev")) return null_ptr;
    if (rejectNull(out_ctx, "bind_i2c: out_ctx")) return null_ptr;
    if (rejectNull(ops, "bind_i2c: ops")) return null_ptr;
    const seam = ops.?;
    if (rejectNull(@ptrCast(seam.write), "bind_i2c: ops.write")) return null_ptr;
    if (rejectNull(@ptrCast(seam.transfer), "bind_i2c: ops.transfer")) return null_ptr;
    if (target_7b > framing.addr_7b_max) return invalid_arg;

    const binding = out_ctx.?;
    binding.* = .{ .bus = seam.*, .target_7b = target_7b };

    const bus = driver.Bus{
        .read_regs = readTrampoline,
        .write_regs = writeTrampoline,
        .ctx = binding,
    };
    const bound = driver.ra8_lsm6dso_init(out_dev.?, &bus);
    if (bound != ok) {
        binding.* = .{};
    }
    return bound;
}
