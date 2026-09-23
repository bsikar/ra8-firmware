//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for `libs/ra8_ov5640/inc/ra8_ov5640.h`. The register map,
//! the VGA scene table and the status decode arithmetic live in
//! `internal/root.zig`; this file owns the exported symbols, the caller-owned
//! `ra8_ov5640_t` and `ra8_ov5640_bus_t` layouts, the argument guards in their
//! original order and the `ra8_err_t` mapping.
//!
//! The SCCB transport and the millisecond delay stay caller-supplied seams
//! (Dependency Inversion), so this library links against no RA8 peripheral and
//! the host suite's register-file mock substitutes exactly as it did under C.

const std = @import("std");
const core = @import("internal/root.zig");

/// `ra8_ov5640_jpeg_status_t`.
pub const JpegStatus = core.JpegStatus;

/// Subset of `ra8_err_t` this library returns on its own behalf. Transport
/// codes (for example `k_ra8_err_nack`, 0x407) pass through untouched.
pub const Ov5640Error = enum(u16) {
    ok = 0,
    invalid_arg = 0x103,
    not_found = 0x106,
    not_supported = 0x107,
    not_initialized = 0x10F,
    null_ptr = 0x504,
};

const ok: u16 = @intFromEnum(Ov5640Error.ok);

/// Component tag on this library's log lines, matching the C's call sites.
const tag: [*:0]const u8 = "ov5640";

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;

/// `ra8_ov5640_read_fn_t`.
pub const ReadFn = *const fn (ctx: ?*anyopaque, address: u8, reg: u16, out_value: ?*u8) callconv(.c) u16;
/// `ra8_ov5640_write_fn_t`.
pub const WriteFn = *const fn (ctx: ?*anyopaque, address: u8, reg: u16, value: u8) callconv(.c) u16;
/// `ra8_ov5640_delay_fn_t`.
pub const DelayFn = *const fn (ctx: ?*anyopaque, milliseconds: u32) callconv(.c) void;

/// `ra8_ov5640_bus_t`: the caller-owned transport and timing interface.
pub const Bus = extern struct {
    read_reg: ?ReadFn = null,
    write_reg: ?WriteFn = null,
    delay_ms: ?DelayFn = null,
    ctx: ?*anyopaque = null,
};

/// `ra8_ov5640_t`: the caller-owned sensor instance.
pub const Device = extern struct {
    bus: Bus = .{},
    address: u8 = 0,
    initialized: bool = false,
};

comptime {
    const word = @sizeOf(usize);
    // Bus is four pointers, in header order.
    std.debug.assert(@sizeOf(Bus) == word * 4);
    std.debug.assert(@offsetOf(Bus, "read_reg") == 0);
    std.debug.assert(@offsetOf(Bus, "write_reg") == word);
    std.debug.assert(@offsetOf(Bus, "delay_ms") == word * 2);
    std.debug.assert(@offsetOf(Bus, "ctx") == word * 3);
    // Device is the bus followed by two bytes, which share one trailing
    // pointer-sized slot on both the host and 32-bit Arm.
    std.debug.assert(@sizeOf(Device) == word * 5);
    std.debug.assert(@offsetOf(Device, "bus") == 0);
    std.debug.assert(@offsetOf(Device, "address") == word * 4);
    std.debug.assert(@offsetOf(Device, "initialized") == word * 4 + 1);
}

/// Reject a `nullptr` argument with the same tag and message the C emitted.
fn rejectNull(pointer: ?*const anyopaque, message: [*:0]const u8) u8 {
    if (pointer == null) {
        ra8_log_emit_error(tag, message);
        return 1;
    }
    return 0;
}

/// Read one register through the bound transport, with the seam guards the
/// public entry point applies.
fn readRegister(device: *Device, register: u16, out_value: *u8) u16 {
    const read_fn = device.bus.read_reg orelse {
        ra8_log_emit_error(tag, "read");
        return @intFromEnum(Ov5640Error.null_ptr);
    };
    return read_fn(device.bus.ctx, device.address, register, out_value);
}

/// Write one register through the bound transport.
fn writeRegister(device: *Device, register: u16, value: u8) u16 {
    const write_fn = device.bus.write_reg orelse {
        ra8_log_emit_error(tag, "write");
        return @intFromEnum(Ov5640Error.null_ptr);
    };
    return write_fn(device.bus.ctx, device.address, register, value);
}

/// Ask the injected callback for a settle delay.
fn waitMilliseconds(device: *Device, milliseconds: u32) void {
    if (device.bus.delay_ms) |delay_fn| {
        delay_fn(device.bus.ctx, milliseconds);
    }
}

/// Read-modify-write one register, preserving the bits outside `mask`.
fn updateBits(device: *Device, register: u16, mask: u8, value: u8) u16 {
    var current: u8 = 0;
    const read_status = readRegister(device, register, &current);
    if (read_status != ok) {
        return read_status;
    }
    return writeRegister(device, register, core.mergeBits(current, mask, value));
}

/// Verify a bounded set of masked register expectations.
fn verify(device: *Device, expectations: []const core.RegExpect) u16 {
    for (expectations) |expectation| {
        var actual: u8 = 0;
        const status = readRegister(device, expectation.reg, &actual);
        if (status != ok) {
            return status;
        }
        if (!core.expectationMatches(actual, expectation)) {
            return @intFromEnum(Ov5640Error.invalid_arg);
        }
    }
    return ok;
}

/// Read and combine the two chip-ID bytes at the selected address.
fn readChipId(device: *Device, out_id: *u16) u16 {
    var hi: u8 = 0;
    var lo: u8 = 0;
    var status = readRegister(device, core.reg.chip_id_hi, &hi);
    if (status == ok) {
        status = readRegister(device, core.reg.chip_id_lo, &lo);
    }
    if (status == ok) {
        out_id.* = core.combineId(hi, lo);
    }
    return status;
}

/// Program the board-qualified VGA base table, applying the MCU-reset delay
/// at the row that holds it.
fn writeVgaBase(device: *Device) u16 {
    for (core.vga_uyvy) |row| {
        const status = writeRegister(device, row.reg, row.val);
        if (status != ok) {
            return status;
        }
        if (core.needsMcuResetDelay(row.reg)) {
            waitMilliseconds(device, core.delay.mcu_reset_ms);
        }
    }
    return ok;
}

/// Apply the JPEG overlay to the VGA base scene.
fn configureJpeg(device: *Device) u16 {
    for (core.jpeg_writes) |row| {
        const status = writeRegister(device, row.reg, row.val);
        if (status != ok) {
            return status;
        }
    }
    var status = updateBits(
        device,
        core.reg.polarity_ctrl00,
        core.val.jpeg_sync_polarity_mask,
        core.val.jpeg_sync_polarity,
    );
    if (status == ok) {
        status = updateBits(
            device,
            core.reg.timing_tc_reg21,
            core.val.jpeg_enable_mask,
            core.val.jpeg_enable_mask,
        );
    }
    if (status == ok) {
        status = updateBits(device, core.reg.system_reset02, core.val.jpeg_reset_mask, 0);
    }
    if (status == ok) {
        status = updateBits(
            device,
            core.reg.clock_enable02,
            core.val.jpeg_clock_mask,
            core.val.jpeg_clock_mask,
        );
    }
    if (status == ok) {
        status = writeRegister(device, core.reg.system_reset00, core.val.mcu_reset_hold);
    }
    return status;
}

/// Read every register of one JPEG status snapshot, in the C's fixed order.
fn readJpegStatus(device: *Device, raw: *core.JpegStatusRaw) u16 {
    const order = [_]struct { reg: u16, field: *u8 }{
        .{ .reg = core.reg.jpeg_length_hi, .field = &raw.length_hi },
        .{ .reg = core.reg.jpeg_length_mid, .field = &raw.length_mid },
        .{ .reg = core.reg.jpeg_length_lo, .field = &raw.length_lo },
        .{ .reg = core.reg.jfifo_overflow, .field = &raw.overflow },
        .{ .reg = core.reg.jpeg_ctrl00, .field = &raw.jpeg_input },
        .{ .reg = core.reg.jpeg_ctrl01, .field = &raw.jpeg_ctrl01 },
        .{ .reg = core.reg.jpeg_ctrl04, .field = &raw.jpeg_header },
        .{ .reg = core.reg.vfifo_ctrl00, .field = &raw.vfifo_ctrl00 },
        .{ .reg = core.reg.compression_w_hi, .field = &raw.width_hi },
        .{ .reg = core.reg.compression_w_lo, .field = &raw.width_lo },
        .{ .reg = core.reg.compression_h_hi, .field = &raw.height_hi },
        .{ .reg = core.reg.compression_h_lo, .field = &raw.height_lo },
        .{ .reg = core.reg.href_minimum, .field = &raw.href_minimum },
        .{ .reg = core.reg.timing_tc_reg21, .field = &raw.timing_ctrl21 },
    };
    for (order) |entry| {
        const status = readRegister(device, entry.reg, entry.field);
        if (status != ok) {
            return status;
        }
    }
    return ok;
}

/// `ra8_ov5640_init`: bind a transport without touching the sensor.
pub export fn ra8_ov5640_init(dev: ?*Device, bus: ?*const Bus) callconv(.c) u16 {
    if (rejectNull(dev, "init") != 0) return @intFromEnum(Ov5640Error.null_ptr);
    if (rejectNull(bus, "init") != 0) return @intFromEnum(Ov5640Error.null_ptr);
    const device = dev.?;
    const source = bus.?;
    if (rejectNull(@ptrCast(source.read_reg), "init") != 0) return @intFromEnum(Ov5640Error.null_ptr);
    if (rejectNull(@ptrCast(source.write_reg), "init") != 0) return @intFromEnum(Ov5640Error.null_ptr);
    if (rejectNull(@ptrCast(source.delay_ms), "init") != 0) return @intFromEnum(Ov5640Error.null_ptr);
    device.* = .{
        .bus = source.*,
        .address = core.addresses[0],
        .initialized = true,
    };
    return ok;
}

/// `ra8_ov5640_read_reg`: one SCCB read at the selected address.
pub export fn ra8_ov5640_read_reg(dev: ?*Device, reg: u16, out_value: ?*u8) callconv(.c) u16 {
    if (rejectNull(dev, "read") != 0) return @intFromEnum(Ov5640Error.null_ptr);
    if (rejectNull(out_value, "read") != 0) return @intFromEnum(Ov5640Error.null_ptr);
    const device = dev.?;
    if (!device.initialized) {
        return @intFromEnum(Ov5640Error.not_initialized);
    }
    return readRegister(device, reg, out_value.?);
}

/// `ra8_ov5640_write_reg`: one SCCB write at the selected address.
pub export fn ra8_ov5640_write_reg(dev: ?*Device, reg: u16, value: u8) callconv(.c) u16 {
    if (rejectNull(dev, "write") != 0) return @intFromEnum(Ov5640Error.null_ptr);
    const device = dev.?;
    if (!device.initialized) {
        return @intFromEnum(Ov5640Error.not_initialized);
    }
    return writeRegister(device, reg, value);
}

/// `ra8_ov5640_probe`: try both legal addresses and verify the chip ID.
pub export fn ra8_ov5640_probe(dev: ?*Device, out_id: ?*u16) callconv(.c) u16 {
    if (rejectNull(dev, "probe") != 0) return @intFromEnum(Ov5640Error.null_ptr);
    if (rejectNull(out_id, "probe") != 0) return @intFromEnum(Ov5640Error.null_ptr);
    const device = dev.?;
    if (!device.initialized) {
        return @intFromEnum(Ov5640Error.not_initialized);
    }
    const reported = out_id.?;
    reported.* = 0;
    for (core.addresses) |address| {
        device.address = address;
        var id: u16 = 0;
        if ((readChipId(device, &id) == ok) and (id == core.chip_id)) {
            reported.* = id;
            return ok;
        }
        reported.* = id;
    }
    device.address = core.addresses[0];
    return @intFromEnum(Ov5640Error.not_found);
}

/// `ra8_ov5640_configure`: reset, program one validated mode, verify it.
pub export fn ra8_ov5640_configure(dev: ?*Device, mode: u8) callconv(.c) u16 {
    if (rejectNull(dev, "configure") != 0) return @intFromEnum(Ov5640Error.null_ptr);
    const device = dev.?;
    if (!device.initialized) {
        return @intFromEnum(Ov5640Error.not_initialized);
    }
    if (!core.modeSupported(mode)) {
        return @intFromEnum(Ov5640Error.not_supported);
    }
    waitMilliseconds(device, core.delay.reset_guard_ms);
    var status = writeRegister(device, core.reg.sw_reset, core.val.sw_reset_hold);
    if (status != ok) {
        return status;
    }
    waitMilliseconds(device, core.delay.reset_guard_ms);
    status = writeVgaBase(device);
    // Verbatim from the C: the JPEG overlay replaces the base-table status
    // rather than short-circuiting on it, so a base-table fault in JPEG mode
    // is reported only if the overlay faults too.
    if (core.verifiesJpeg(mode)) {
        status = configureJpeg(device);
    }
    if (status != ok) {
        return status;
    }
    waitMilliseconds(device, core.delay.cfg_settle_ms);
    status = verify(device, &core.uyvy_expect);
    if ((status != ok) or !core.verifiesJpeg(mode)) {
        return status;
    }
    return verify(device, &core.jpeg_expect);
}

/// `ra8_ov5640_set_jpeg_quantization_scale`: program CTRL07 bits [5:0].
pub export fn ra8_ov5640_set_jpeg_quantization_scale(dev: ?*Device, quant_scale: u8) callconv(.c) u16 {
    if (rejectNull(dev, "jpeg_quality") != 0) return @intFromEnum(Ov5640Error.null_ptr);
    const device = dev.?;
    if (!device.initialized) {
        return @intFromEnum(Ov5640Error.not_initialized);
    }
    if (!core.quantScaleValid(quant_scale)) {
        return @intFromEnum(Ov5640Error.invalid_arg);
    }
    return updateBits(device, core.reg.jpeg_quality, core.val.jpeg_quant_scale_mask, quant_scale);
}

/// `ra8_ov5640_jpeg_status_get`: snapshot the JPEG pipeline registers.
pub export fn ra8_ov5640_jpeg_status_get(dev: ?*Device, out_status: ?*JpegStatus) callconv(.c) u16 {
    if (rejectNull(dev, "jpeg_status") != 0) return @intFromEnum(Ov5640Error.null_ptr);
    if (rejectNull(out_status, "jpeg_status") != 0) return @intFromEnum(Ov5640Error.null_ptr);
    const device = dev.?;
    const destination = out_status.?;
    // The C zeroes the snapshot before the state guard runs.
    destination.* = .{};
    if (!device.initialized) {
        return @intFromEnum(Ov5640Error.not_initialized);
    }
    var raw: core.JpegStatusRaw = .{};
    const status = readJpegStatus(device, &raw);
    if (status != ok) {
        return status;
    }
    destination.* = core.decodeJpegStatus(raw);
    return ok;
}

/// `ra8_ov5640_stream_set`: enter software standby or resume streaming.
pub export fn ra8_ov5640_stream_set(dev: ?*Device, enabled: bool) callconv(.c) u16 {
    // The C rejected a null device here with a bare comparison, emitting no
    // log line, unlike every other entry point. Kept verbatim.
    const device = dev orelse return @intFromEnum(Ov5640Error.null_ptr);
    if (!device.initialized) {
        return @intFromEnum(Ov5640Error.not_initialized);
    }
    const status = writeRegister(device, core.reg.sw_reset, core.streamValue(enabled));
    if (status == ok) {
        waitMilliseconds(device, core.delay.stream_settle_ms);
    }
    return status;
}
