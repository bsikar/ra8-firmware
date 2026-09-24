//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for `libs/ra8_lsm6dso/inc/ra8_lsm6dso.h`. The bit-field
//! encoders, sample decoders and FIFO arithmetic live in `internal/root.zig`;
//! this file owns the exported symbols, the caller-owned `ra8_lsm6dso_t` and
//! `ra8_lsm6dso_bus_t` layouts, the argument guards in their original order
//! and the `ra8_err_t` mapping.
//!
//! The transport stays a caller-supplied seam (Dependency Inversion): the
//! driver never links against `ra8_i3c_i2c` or `ra8_spi`, so the host suite
//! canned-response mock substitutes exactly as it did under C.

const std = @import("std");
const implementation = @import("internal/root.zig");

/// Three-axis raw sample (`ra8_lsm6dso_xyz_t`).
pub const Xyz = implementation.Xyz;

/// Subset of `ra8_err_t` this library returns on its own behalf. Transport
/// codes (for example `k_ra8_err_nack`, 0x407) pass through untouched.
pub const Lsm6dsoError = enum(u16) {
    ok = 0,
    invalid_arg = 0x103,
    not_initialized = 0x10F,
    null_ptr = 0x504,
};

/// Component tag on this library log lines, matching the C `s_lsm6dso_tag`.
const tag: [*:0]const u8 = "lsm6dso";

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;

/// `ra8_lsm6dso_read_fn_t`.
pub const ReadFn = *const fn (ctx: ?*anyopaque, reg: u8, buf: ?[*]u8, len: u32) callconv(.c) u16;
/// `ra8_lsm6dso_write_fn_t`.
pub const WriteFn = *const fn (ctx: ?*anyopaque, reg: u8, buf: ?[*]const u8, len: u32) callconv(.c) u16;

/// `ra8_lsm6dso_bus_t`: the caller-owned transport interface.
pub const Bus = extern struct {
    read_regs: ?ReadFn = null,
    write_regs: ?WriteFn = null,
    ctx: ?*anyopaque = null,
};

/// `ra8_lsm6dso_t`: the caller-owned driver instance.
pub const Device = extern struct {
    bus: Bus = .{},
    accel_fs_code: u8 = 0,
    gyro_fs_code: u8 = 0,
    odr_code: u8 = 0,
    initialized: bool = false,
};

comptime {
    const word = @sizeOf(usize);
    // Bus is three pointers, in header order.
    std.debug.assert(@sizeOf(Bus) == word * 3);
    std.debug.assert(@offsetOf(Bus, "read_regs") == 0);
    std.debug.assert(@offsetOf(Bus, "write_regs") == word);
    std.debug.assert(@offsetOf(Bus, "ctx") == word * 2);
    // Device is the bus followed by three enum bytes and the init flag, which
    // all fit inside one trailing pointer-sized slot on host and on Arm.
    std.debug.assert(@sizeOf(Device) == word * 4);
    std.debug.assert(@offsetOf(Device, "bus") == 0);
    std.debug.assert(@offsetOf(Device, "accel_fs_code") == word * 3);
    std.debug.assert(@offsetOf(Device, "gyro_fs_code") == word * 3 + 1);
    std.debug.assert(@offsetOf(Device, "odr_code") == word * 3 + 2);
    std.debug.assert(@offsetOf(Device, "initialized") == word * 3 + 3);
    // The XYZ sample is packed 3 x int16 with no padding.
    std.debug.assert(@sizeOf(Xyz) == 6);
    std.debug.assert(@offsetOf(Xyz, "y") == 2);
    std.debug.assert(@offsetOf(Xyz, "z") == 4);
}

const ok = @intFromEnum(Lsm6dsoError.ok);

/// Reject a NULL argument with the C log line and `ra8_err_t` code.
fn rejectNull(message: [*:0]const u8) u16 {
    ra8_log_emit_error(tag, message);
    return @intFromEnum(Lsm6dsoError.null_ptr);
}

/// Reject a call made before `ra8_lsm6dso_init`.
fn rejectUninitialized(message: [*:0]const u8) u16 {
    ra8_log_emit_error(tag, message);
    return @intFromEnum(Lsm6dsoError.not_initialized);
}

/// Reject an out-of-range enumerator. `RA8_CHECK_RANGE_TAG` logs this exact
/// fixed string rather than a per-call message, so the port keeps it.
fn rejectRange() u16 {
    ra8_log_emit_error(tag, "Range check failed");
    return @intFromEnum(Lsm6dsoError.invalid_arg);
}

/// Transport read. The seam pointers are non-NULL for any device that came
/// through `ra8_lsm6dso_init`; answering `null_ptr` on a hand-built device is
/// a hardening over the C, which dereferenced the slot unconditionally.
fn busRead(dev: *Device, reg: u8, buf: [*]u8, len: u32) u16 {
    const read_regs = dev.bus.read_regs orelse {
        return rejectNull("bus: read_regs");
    };
    return read_regs(dev.bus.ctx, reg, buf, len);
}

/// Transport write, with the same hardening as `busRead`.
fn busWrite(dev: *Device, reg: u8, buf: [*]const u8, len: u32) u16 {
    const write_regs = dev.bus.write_regs orelse {
        return rejectNull("bus: write_regs");
    };
    return write_regs(dev.bus.ctx, reg, buf, len);
}

/// Read one register byte.
fn readByte(dev: *Device, reg: u8, out: *u8) u16 {
    return busRead(dev, reg, @ptrCast(out), 1);
}

/// Write one register byte. The value is staged on this frame so the
/// transport may DMA out of it, exactly as the C helper did.
fn writeByte(dev: *Device, reg: u8, value: u8) u16 {
    var staged = value;
    return busWrite(dev, reg, @ptrCast(&staged), 1);
}

/// Read-modify-write the ODR nibble [7:4] of a CTRL register.
fn rmwOdr(dev: *Device, reg: u8, odr_bits: u8) u16 {
    var now: u8 = 0;
    const r = readByte(dev, reg, &now);
    if (r != ok) {
        return r;
    }
    return writeByte(dev, reg, implementation.odrMerge(now, odr_bits));
}

/// Bind a transport and reset the cached configuration to the DS12140
/// power-on defaults.
pub export fn ra8_lsm6dso_init(out_dev: ?*Device, bus: ?*const Bus) callconv(.c) u16 {
    const dev = out_dev orelse return rejectNull("init: out_dev");
    const source = bus orelse return rejectNull("init: bus");
    if (source.read_regs == null) {
        return rejectNull("init: bus.read_regs");
    }
    if (source.write_regs == null) {
        return rejectNull("init: bus.write_regs");
    }

    dev.bus = source.*;
    dev.accel_fs_code = 0x00; // k_lsm6dso_xl_fs_2g, sec 9.12 reset default
    dev.gyro_fs_code = implementation.g_fs_250dps; // sec 9.13 reset default
    dev.odr_code = 0x00; // k_lsm6dso_odr_off, sec 9.12 reset default
    dev.initialized = true;
    return ok;
}

/// Read WHO_AM_I (sec 9.11); a genuine part answers 0x6C.
pub export fn ra8_lsm6dso_who_am_i(dev: ?*Device, out_id: ?*u8) callconv(.c) u16 {
    const device = dev orelse return rejectNull("who_am_i: dev");
    const out = out_id orelse return rejectNull("who_am_i: out_id");
    if (!device.initialized) {
        return rejectUninitialized("who_am_i: not initialized");
    }
    return readByte(device, implementation.reg_who_am_i, out);
}

/// Program FS_XL[3:2] of CTRL1_XL, leaving the ODR nibble alone.
pub export fn ra8_lsm6dso_set_accel_range(dev: ?*Device, fs: u8) callconv(.c) u16 {
    const device = dev orelse return rejectNull("set_accel_range: dev");
    if (!device.initialized) {
        return rejectUninitialized("set_accel_range: not initialized");
    }
    if (fs > implementation.xl_fs_cap) {
        return rejectRange();
    }

    var current: u8 = 0;
    const r = readByte(device, implementation.reg_ctrl1_xl, &current);
    if (r != ok) {
        return r;
    }
    const w = writeByte(
        device,
        implementation.reg_ctrl1_xl,
        implementation.accelFsMerge(current, fs),
    );
    if (w != ok) {
        return w;
    }
    device.accel_fs_code = fs;
    return ok;
}

/// Program FS_G[3:2] + FS_125[1] of CTRL2_G, leaving the ODR nibble alone.
pub export fn ra8_lsm6dso_set_gyro_range(dev: ?*Device, fs: u8) callconv(.c) u16 {
    const device = dev orelse return rejectNull("set_gyro_range: dev");
    if (!device.initialized) {
        return rejectUninitialized("set_gyro_range: not initialized");
    }
    if (fs > implementation.g_fs_cap) {
        return rejectRange();
    }

    var current: u8 = 0;
    const r = readByte(device, implementation.reg_ctrl2_g, &current);
    if (r != ok) {
        return r;
    }
    const w = writeByte(
        device,
        implementation.reg_ctrl2_g,
        implementation.gyroFsMerge(current, fs),
    );
    if (w != ok) {
        return w;
    }
    device.gyro_fs_code = fs;
    return ok;
}

/// Program the shared ODR nibble into CTRL1_XL and then CTRL2_G.
pub export fn ra8_lsm6dso_set_odr(dev: ?*Device, odr: u8) callconv(.c) u16 {
    const device = dev orelse return rejectNull("set_odr: dev");
    if (!device.initialized) {
        return rejectUninitialized("set_odr: not initialized");
    }
    if (odr > implementation.odr_cap) {
        return rejectRange();
    }

    const odr_bits = implementation.odrBits(odr);
    const rxl = rmwOdr(device, implementation.reg_ctrl1_xl, odr_bits);
    if (rxl != ok) {
        return rxl;
    }
    const rg = rmwOdr(device, implementation.reg_ctrl2_g, odr_bits);
    if (rg != ok) {
        return rg;
    }
    device.odr_code = odr;
    return ok;
}

/// Burst six bytes at `reg` and pack them into a caller sample.
fn readXyz(device: *Device, reg: u8, out: *Xyz) u16 {
    var bytes: [6]u8 = @splat(0);
    const r = busRead(device, reg, &bytes, implementation.xyz_burst_bytes);
    if (r != ok) {
        return r;
    }
    out.* = implementation.unpackXyz(&bytes);
    return ok;
}

/// Read the latest accelerometer sample (sec 9.35, OUTX_L_A auto-increment).
pub export fn ra8_lsm6dso_read_accel(dev: ?*Device, out: ?*Xyz) callconv(.c) u16 {
    const device = dev orelse return rejectNull("read_accel: dev");
    const sample = out orelse return rejectNull("read_accel: out");
    if (!device.initialized) {
        return rejectUninitialized("read_accel: not initialized");
    }
    return readXyz(device, implementation.reg_outx_l_a, sample);
}

/// Read the latest gyroscope sample (sec 9.29, OUTX_L_G auto-increment).
pub export fn ra8_lsm6dso_read_gyro(dev: ?*Device, out: ?*Xyz) callconv(.c) u16 {
    const device = dev orelse return rejectNull("read_gyro: dev");
    const sample = out orelse return rejectNull("read_gyro: out");
    if (!device.initialized) {
        return rejectUninitialized("read_gyro: not initialized");
    }
    return readXyz(device, implementation.reg_outx_l_g, sample);
}

/// Read the die temperature in centi-degrees Celsius (sec 9.27 / 9.28).
pub export fn ra8_lsm6dso_read_temp(dev: ?*Device, out_centi_c: ?*i32) callconv(.c) u16 {
    const device = dev orelse return rejectNull("read_temp: dev");
    const out = out_centi_c orelse return rejectNull("read_temp: out_centi_c");
    if (!device.initialized) {
        return rejectUninitialized("read_temp: not initialized");
    }

    var bytes: [2]u8 = @splat(0);
    const r = busRead(
        device,
        implementation.reg_out_temp_l,
        &bytes,
        implementation.temp_burst_bytes,
    );
    if (r != ok) {
        return r;
    }
    out.* = implementation.tempCentiC(implementation.combineLe(bytes[0], bytes[1]));
    return ok;
}

/// Single validation gate for the FIFO drain, in C guard order.
fn fifoCheckArgs(dev: ?*Device, out_buf: ?[*]u8, max_words: u32, out_words: ?*u32) u16 {
    const device = dev orelse return rejectNull("read_fifo: dev");
    if (out_buf == null) {
        return rejectNull("read_fifo: out_buf");
    }
    if (out_words == null) {
        return rejectNull("read_fifo: out_words");
    }
    if (!device.initialized) {
        return rejectUninitialized("read_fifo: not initialized");
    }
    if (max_words == 0) {
        ra8_log_emit_error(tag, "read_fifo: max_words is zero");
        return @intFromEnum(Lsm6dsoError.invalid_arg);
    }
    return ok;
}

/// Drain up to `max_words` 7-byte FIFO records (sec 9.44 depth, sec 9.60 data).
pub export fn ra8_lsm6dso_read_xl_gyro_fifo(
    dev: ?*Device,
    out_buf: ?[*]u8,
    max_words: u32,
    out_words: ?*u32,
) callconv(.c) u16 {
    // Set the output count early so callers can rely on it on failure, before
    // any other guard runs. The C did this first too.
    if (out_words) |words| {
        words.* = 0;
    }
    const check = fifoCheckArgs(dev, out_buf, max_words, out_words);
    if (check != ok) {
        return check;
    }
    const device = dev.?;
    const buffer = out_buf.?;
    const words = out_words.?;

    var status: [2]u8 = @splat(0);
    const rs = busRead(
        device,
        implementation.reg_fifo_status1,
        &status,
        implementation.fifo_status_bytes,
    );
    if (rs != ok) {
        return rs;
    }
    const live = implementation.fifoDepth(status[0], status[1]);
    const to_read = implementation.wordsToRead(live, max_words);
    if (to_read == 0) {
        return ok;
    }
    const rd = busRead(
        device,
        implementation.reg_fifo_data_out,
        buffer,
        implementation.fifoTotalBytes(to_read),
    );
    if (rd != ok) {
        return rd;
    }
    words.* = to_read;
    return ok;
}
