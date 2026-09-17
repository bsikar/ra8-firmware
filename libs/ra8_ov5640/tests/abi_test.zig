//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Zig-native tests for the OV5640 C ABI membrane. A register-file mock
//! substitutes for the SCCB transport exactly as the host C suite's fixture
//! does, and this file exports its own counting `ra8_log_emit_error` sink so
//! each guard's tag and message can be asserted.

const std = @import("std");
const testing = std.testing;
const abi = @import("abi");

var log_count: u32 = 0;
var last_tag: [*:0]const u8 = "";
var last_message: [*:0]const u8 = "";

/// Link-time substitute for the real `ra8_core` log sink.
export fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) callconv(.c) void {
    log_count += 1;
    last_tag = tag;
    last_message = message;
}

fn lastMessageIs(expected: []const u8) bool {
    return std.mem.eql(u8, std.mem.span(last_message), expected);
}

fn lastTagIs(expected: []const u8) bool {
    return std.mem.eql(u8, std.mem.span(last_tag), expected);
}

const no_failure: u32 = std.math.maxInt(u32);
const nack: u16 = 0x407;
const null_ptr: u16 = 0x504;
const invalid_arg: u16 = 0x103;
const not_found: u16 = 0x106;
const not_supported: u16 = 0x107;
const not_initialized: u16 = 0x10F;

const WriteRecord = struct { address: u8 = 0, reg: u16 = 0, value: u8 = 0 };

/// Mock SCCB device: a flat register file plus the fixture's injection knobs.
const Mock = struct {
    regs: [0x10000]u8 = [_]u8{0} ** 0x10000,
    writes: [512]WriteRecord = [_]WriteRecord{.{}} ** 512,
    delays: [64]u32 = [_]u32{0} ** 64,
    read_count: u32 = 0,
    write_count: u32 = 0,
    delay_count: u32 = 0,
    fail_write_at: u32 = no_failure,
    forced_read_error: u16 = 0,
    primary_present: bool = true,
    secondary_present: bool = false,
    corrupt_read: bool = false,
    corrupt_reg: u16 = 0,
    corrupt_value: u8 = 0,

    fn reset(self: *Mock) void {
        self.* = .{};
        self.regs[0x300A] = 0x56;
        self.regs[0x300B] = 0x40;
    }

    fn present(self: *Mock, address: u8) bool {
        if (address == 0x3C) return self.primary_present;
        if (address == 0x3D) return self.secondary_present;
        return false;
    }
};

var mock: Mock = .{};

fn mockRead(ctx: ?*anyopaque, address: u8, reg: u16, out_value: ?*u8) callconv(.c) u16 {
    _ = ctx;
    mock.read_count += 1;
    if (mock.forced_read_error != 0) return mock.forced_read_error;
    if (!mock.present(address)) return nack;
    out_value.?.* = if (mock.corrupt_read and reg == mock.corrupt_reg)
        mock.corrupt_value
    else
        mock.regs[reg];
    return 0;
}

fn mockWrite(ctx: ?*anyopaque, address: u8, reg: u16, value: u8) callconv(.c) u16 {
    _ = ctx;
    if (mock.write_count == mock.fail_write_at) return nack;
    if (!mock.present(address)) return nack;
    if (mock.write_count < mock.writes.len) {
        mock.writes[mock.write_count] = .{ .address = address, .reg = reg, .value = value };
    }
    mock.write_count += 1;
    mock.regs[reg] = value;
    return 0;
}

fn mockDelay(ctx: ?*anyopaque, milliseconds: u32) callconv(.c) void {
    _ = ctx;
    if (mock.delay_count < mock.delays.len) mock.delays[mock.delay_count] = milliseconds;
    mock.delay_count += 1;
}

fn freshBus() abi.Bus {
    return .{
        .read_reg = &mockRead,
        .write_reg = &mockWrite,
        .delay_ms = &mockDelay,
        .ctx = null,
    };
}

/// Reset the fixture and hand back an initialized device.
fn freshDevice() abi.Device {
    mock.reset();
    log_count = 0;
    var device: abi.Device = .{};
    const bus = freshBus();
    std.debug.assert(abi.ra8_ov5640_init(&device, &bus) == 0);
    return device;
}

test "init rejects a null device and logs the init guard" {
    mock.reset();
    log_count = 0;
    const bus = freshBus();
    try testing.expectEqual(null_ptr, abi.ra8_ov5640_init(null, &bus));
    try testing.expectEqual(@as(u32, 1), log_count);
    try testing.expect(lastTagIs("ov5640"));
    try testing.expect(lastMessageIs("init"));
}

test "init rejects a null bus" {
    mock.reset();
    log_count = 0;
    var device: abi.Device = .{};
    try testing.expectEqual(null_ptr, abi.ra8_ov5640_init(&device, null));
    try testing.expectEqual(@as(u32, 1), log_count);
    try testing.expect(lastMessageIs("init"));
}

test "init rejects each missing transport callback in header order" {
    mock.reset();
    var device: abi.Device = .{};
    var bus = freshBus();

    bus.read_reg = null;
    log_count = 0;
    try testing.expectEqual(null_ptr, abi.ra8_ov5640_init(&device, &bus));
    try testing.expectEqual(@as(u32, 1), log_count);

    bus = freshBus();
    bus.write_reg = null;
    try testing.expectEqual(null_ptr, abi.ra8_ov5640_init(&device, &bus));

    bus = freshBus();
    bus.delay_ms = null;
    try testing.expectEqual(null_ptr, abi.ra8_ov5640_init(&device, &bus));
    try testing.expect(!device.initialized);
}

test "init copies the transport, selects the primary address and performs no io" {
    const device = freshDevice();
    try testing.expect(device.initialized);
    try testing.expectEqual(@as(u8, 0x3C), device.address);
    try testing.expectEqual(@as(u32, 0), mock.read_count);
    try testing.expectEqual(@as(u32, 0), mock.write_count);
    try testing.expectEqual(@as(u32, 0), mock.delay_count);
}

test "read_reg rejects a null device then a null destination" {
    mock.reset();
    var value: u8 = 0;
    log_count = 0;
    try testing.expectEqual(null_ptr, abi.ra8_ov5640_read_reg(null, 0x300A, &value));
    try testing.expect(lastMessageIs("read"));

    var device = freshDevice();
    log_count = 0;
    try testing.expectEqual(null_ptr, abi.ra8_ov5640_read_reg(&device, 0x300A, null));
    try testing.expectEqual(@as(u32, 1), log_count);
    try testing.expect(lastMessageIs("read"));
}

test "read_reg answers not_initialized before touching the transport" {
    mock.reset();
    var device: abi.Device = .{ .bus = freshBus() };
    var value: u8 = 0;
    try testing.expectEqual(not_initialized, abi.ra8_ov5640_read_reg(&device, 0x300A, &value));
    try testing.expectEqual(@as(u32, 0), mock.read_count);
}

test "read_reg returns the register byte through the bound transport" {
    var device = freshDevice();
    mock.regs[0x4407] = 0x0C;
    var value: u8 = 0;
    try testing.expectEqual(@as(u16, 0), abi.ra8_ov5640_read_reg(&device, 0x4407, &value));
    try testing.expectEqual(@as(u8, 0x0C), value);
    try testing.expectEqual(@as(u32, 1), mock.read_count);
}

test "read_reg propagates a transport error verbatim" {
    var device = freshDevice();
    mock.forced_read_error = nack;
    var value: u8 = 0;
    try testing.expectEqual(nack, abi.ra8_ov5640_read_reg(&device, 0x4407, &value));
}

test "write_reg rejects a null device and logs the write guard" {
    mock.reset();
    log_count = 0;
    try testing.expectEqual(null_ptr, abi.ra8_ov5640_write_reg(null, 0x4407, 0x01));
    try testing.expectEqual(@as(u32, 1), log_count);
    try testing.expect(lastMessageIs("write"));
}

test "write_reg checks the init state before the transport" {
    mock.reset();
    var device: abi.Device = .{ .bus = freshBus() };
    try testing.expectEqual(not_initialized, abi.ra8_ov5640_write_reg(&device, 0x4407, 0x01));
    try testing.expectEqual(@as(u32, 0), mock.write_count);
}

test "write_reg reaches the register file at the selected address" {
    var device = freshDevice();
    try testing.expectEqual(@as(u16, 0), abi.ra8_ov5640_write_reg(&device, 0x4407, 0x2A));
    try testing.expectEqual(@as(u8, 0x2A), mock.regs[0x4407]);
    try testing.expectEqual(@as(u8, 0x3C), mock.writes[0].address);
}

test "probe rejects a null device then a null id" {
    mock.reset();
    var id: u16 = 0;
    log_count = 0;
    try testing.expectEqual(null_ptr, abi.ra8_ov5640_probe(null, &id));
    try testing.expect(lastMessageIs("probe"));

    var device = freshDevice();
    log_count = 0;
    try testing.expectEqual(null_ptr, abi.ra8_ov5640_probe(&device, null));
    try testing.expectEqual(@as(u32, 1), log_count);
}

test "probe requires an initialized device" {
    mock.reset();
    var device: abi.Device = .{ .bus = freshBus() };
    var id: u16 = 0;
    try testing.expectEqual(not_initialized, abi.ra8_ov5640_probe(&device, &id));
    try testing.expectEqual(@as(u32, 0), mock.read_count);
}

test "probe finds the sensor at the primary address" {
    var device = freshDevice();
    var id: u16 = 0;
    try testing.expectEqual(@as(u16, 0), abi.ra8_ov5640_probe(&device, &id));
    try testing.expectEqual(@as(u16, 0x5640), id);
    try testing.expectEqual(@as(u8, 0x3C), device.address);
    try testing.expectEqual(@as(u32, 2), mock.read_count);
}

test "probe falls through to the secondary address and keeps it" {
    var device = freshDevice();
    mock.primary_present = false;
    mock.secondary_present = true;
    var id: u16 = 0;
    try testing.expectEqual(@as(u16, 0), abi.ra8_ov5640_probe(&device, &id));
    try testing.expectEqual(@as(u16, 0x5640), id);
    try testing.expectEqual(@as(u8, 0x3D), device.address);
}

test "probe reports not_found and restores the primary address" {
    var device = freshDevice();
    mock.primary_present = false;
    mock.secondary_present = false;
    var id: u16 = 0;
    try testing.expectEqual(not_found, abi.ra8_ov5640_probe(&device, &id));
    try testing.expectEqual(@as(u16, 0), id);
    try testing.expectEqual(@as(u8, 0x3C), device.address);
}

test "probe reports the last id it read when both addresses answer wrongly" {
    var device = freshDevice();
    mock.secondary_present = true;
    mock.regs[0x300A] = 0x26;
    mock.regs[0x300B] = 0x45;
    var id: u16 = 0;
    try testing.expectEqual(not_found, abi.ra8_ov5640_probe(&device, &id));
    try testing.expectEqual(@as(u16, 0x2645), id);
    try testing.expectEqual(@as(u32, 4), mock.read_count);
}

test "probe clears the reported id when the last address does not answer" {
    // Verbatim C behaviour: each attempt overwrites the reported id, and a
    // failed chip-ID read leaves its local zero, so a wrong id found at the
    // primary address is clobbered by the silent secondary.
    var device = freshDevice();
    mock.secondary_present = false;
    mock.regs[0x300A] = 0x26;
    mock.regs[0x300B] = 0x45;
    var id: u16 = 0xFFFF;
    try testing.expectEqual(not_found, abi.ra8_ov5640_probe(&device, &id));
    try testing.expectEqual(@as(u16, 0), id);
}

test "configure rejects a null device and an unsupported mode" {
    mock.reset();
    log_count = 0;
    try testing.expectEqual(null_ptr, abi.ra8_ov5640_configure(null, 0));
    try testing.expect(lastMessageIs("configure"));

    var device = freshDevice();
    try testing.expectEqual(not_supported, abi.ra8_ov5640_configure(&device, 2));
    try testing.expectEqual(@as(u32, 0), mock.write_count);
    try testing.expectEqual(@as(u32, 0), mock.delay_count);
}

test "configure checks the init state before the mode" {
    mock.reset();
    var device: abi.Device = .{ .bus = freshBus() };
    try testing.expectEqual(not_initialized, abi.ra8_ov5640_configure(&device, 7));
}

test "configure uyvy writes the base table with the documented delays" {
    var device = freshDevice();
    try testing.expectEqual(@as(u16, 0), abi.ra8_ov5640_configure(&device, 0));
    // One software-reset write plus every base-table row.
    try testing.expectEqual(@as(u32, 272), mock.write_count);
    try testing.expectEqual(@as(u8, 0x82), mock.writes[0].value);
    try testing.expectEqual(@as(u16, 0x3008), mock.writes[0].reg);
    // Two reset guards, the MCU-reset settle and the configuration settle.
    try testing.expectEqual(@as(u32, 4), mock.delay_count);
    try testing.expectEqual(@as(u32, 100), mock.delays[0]);
    try testing.expectEqual(@as(u32, 100), mock.delays[1]);
    try testing.expectEqual(@as(u32, 10), mock.delays[2]);
    try testing.expectEqual(@as(u32, 500), mock.delays[3]);
}

test "configure uyvy leaves the sensor in the verified yuyv scene" {
    var device = freshDevice();
    try testing.expectEqual(@as(u16, 0), abi.ra8_ov5640_configure(&device, 0));
    try testing.expectEqual(@as(u8, 0x30), mock.regs[0x4300]);
    try testing.expectEqual(@as(u8, 0x00), mock.regs[0x501F]);
    try testing.expectEqual(@as(u8, 0x00), mock.regs[0x503D]);
}

test "configure uyvy fails when a verified register reads back corrupt" {
    var device = freshDevice();
    mock.corrupt_read = true;
    mock.corrupt_reg = 0x4300;
    mock.corrupt_value = 0x00;
    try testing.expectEqual(invalid_arg, abi.ra8_ov5640_configure(&device, 0));
}

test "configure jpeg programs the overlay and holds the mcu reset" {
    var device = freshDevice();
    try testing.expectEqual(@as(u16, 0), abi.ra8_ov5640_configure(&device, 1));
    try testing.expectEqual(@as(u8, 0x0C), mock.regs[0x4407] & 0x3F);
    try testing.expectEqual(@as(u8, 0x02), mock.regs[0x4713] & 0x07);
    try testing.expectEqual(@as(u8, 0x20), mock.regs[0x3821] & 0x20);
    try testing.expectEqual(@as(u8, 0x00), mock.regs[0x3002] & 0x1C);
    try testing.expectEqual(@as(u8, 0x28), mock.regs[0x3006] & 0x28);
    try testing.expectEqual(@as(u8, 0x01), mock.regs[0x4740] & 0x03);
    try testing.expectEqual(@as(u8, 0x20), mock.regs[0x3000]);
}

test "configure jpeg fails when the first software reset write nacks" {
    var device = freshDevice();
    mock.fail_write_at = 0;
    try testing.expectEqual(nack, abi.ra8_ov5640_configure(&device, 1));
    try testing.expectEqual(@as(u32, 1), mock.delay_count);
}

test "configure propagates a base-table write fault in uyvy mode" {
    var device = freshDevice();
    mock.fail_write_at = 5;
    try testing.expectEqual(nack, abi.ra8_ov5640_configure(&device, 0));
}

test "configure propagates a read fault raised during verification" {
    const device_init = freshDevice();
    var device = device_init;
    try testing.expectEqual(@as(u16, 0), abi.ra8_ov5640_configure(&device, 0));
    mock.forced_read_error = nack;
    try testing.expectEqual(nack, abi.ra8_ov5640_configure(&device, 0));
}

test "quantization scale rejects a null device and logs jpeg_quality" {
    mock.reset();
    log_count = 0;
    try testing.expectEqual(null_ptr, abi.ra8_ov5640_set_jpeg_quantization_scale(null, 0x10));
    try testing.expectEqual(@as(u32, 1), log_count);
    try testing.expect(lastMessageIs("jpeg_quality"));
}

test "quantization scale checks the init state before the range" {
    mock.reset();
    var device: abi.Device = .{ .bus = freshBus() };
    try testing.expectEqual(not_initialized, abi.ra8_ov5640_set_jpeg_quantization_scale(&device, 0xFF));
}

test "quantization scale rejects a value above the field" {
    var device = freshDevice();
    try testing.expectEqual(invalid_arg, abi.ra8_ov5640_set_jpeg_quantization_scale(&device, 0x40));
    try testing.expectEqual(@as(u32, 0), mock.write_count);
}

test "quantization scale merges into ctrl07 and keeps the unrelated bits" {
    var device = freshDevice();
    mock.regs[0x4407] = 0xC0;
    try testing.expectEqual(@as(u16, 0), abi.ra8_ov5640_set_jpeg_quantization_scale(&device, 0x3F));
    try testing.expectEqual(@as(u8, 0xFF), mock.regs[0x4407]);
    try testing.expectEqual(@as(u16, 0), abi.ra8_ov5640_set_jpeg_quantization_scale(&device, 0x00));
    try testing.expectEqual(@as(u8, 0xC0), mock.regs[0x4407]);
}

test "quantization scale propagates a read fault from the merge" {
    var device = freshDevice();
    mock.forced_read_error = nack;
    try testing.expectEqual(nack, abi.ra8_ov5640_set_jpeg_quantization_scale(&device, 0x10));
}

test "jpeg status rejects a null device then a null destination" {
    mock.reset();
    var status: abi.JpegStatus = .{};
    log_count = 0;
    try testing.expectEqual(null_ptr, abi.ra8_ov5640_jpeg_status_get(null, &status));
    try testing.expect(lastMessageIs("jpeg_status"));

    var device = freshDevice();
    log_count = 0;
    try testing.expectEqual(null_ptr, abi.ra8_ov5640_jpeg_status_get(&device, null));
    try testing.expectEqual(@as(u32, 1), log_count);
}

test "jpeg status zeroes the snapshot before reporting not_initialized" {
    mock.reset();
    var device: abi.Device = .{ .bus = freshBus() };
    var status: abi.JpegStatus = .{ .encoded_bytes = 0xDEAD, .fifo_overflow = true };
    try testing.expectEqual(not_initialized, abi.ra8_ov5640_jpeg_status_get(&device, &status));
    try testing.expectEqual(@as(u32, 0), status.encoded_bytes);
    try testing.expect(!status.fifo_overflow);
}

test "jpeg status decodes every register it read" {
    var device = freshDevice();
    mock.regs[0x4414] = 0x01;
    mock.regs[0x4415] = 0x23;
    mock.regs[0x4416] = 0x45;
    mock.regs[0x4417] = 0x01;
    mock.regs[0x4400] = 0x81;
    mock.regs[0x4401] = 0x5A;
    mock.regs[0x4404] = 0x24;
    mock.regs[0x4600] = 0xA5;
    mock.regs[0x4602] = 0x02;
    mock.regs[0x4603] = 0x80;
    mock.regs[0x4604] = 0x01;
    mock.regs[0x4605] = 0xE0;
    mock.regs[0x471F] = 0x3C;
    mock.regs[0x3821] = 0x21;

    var status: abi.JpegStatus = .{};
    try testing.expectEqual(@as(u16, 0), abi.ra8_ov5640_jpeg_status_get(&device, &status));
    try testing.expectEqual(@as(u32, 0x012345), status.encoded_bytes);
    try testing.expectEqual(@as(u16, 640), status.compression_width);
    try testing.expectEqual(@as(u16, 480), status.compression_height);
    try testing.expectEqual(@as(u8, 0x5A), status.jpeg_ctrl01);
    try testing.expectEqual(@as(u8, 0xA5), status.vfifo_ctrl00);
    try testing.expectEqual(@as(u8, 0x3C), status.href_minimum_blanking);
    try testing.expect(status.fifo_overflow);
    try testing.expect(status.input_is_yuv422);
    try testing.expect(status.header_output);
    try testing.expect(status.compression_enabled);
    // Fourteen registers, read once each, in one fixed order.
    try testing.expectEqual(@as(u32, 14), mock.read_count);
}

test "jpeg status stops at the first transport fault" {
    var device = freshDevice();
    mock.forced_read_error = nack;
    var status: abi.JpegStatus = .{};
    try testing.expectEqual(nack, abi.ra8_ov5640_jpeg_status_get(&device, &status));
    try testing.expectEqual(@as(u32, 1), mock.read_count);
    try testing.expectEqual(@as(u32, 0), status.encoded_bytes);
}

test "stream_set rejects a null device without logging" {
    mock.reset();
    log_count = 0;
    try testing.expectEqual(null_ptr, abi.ra8_ov5640_stream_set(null, true));
    try testing.expectEqual(@as(u32, 0), log_count);
}

test "stream_set requires an initialized device" {
    mock.reset();
    var device: abi.Device = .{ .bus = freshBus() };
    try testing.expectEqual(not_initialized, abi.ra8_ov5640_stream_set(&device, true));
    try testing.expectEqual(@as(u32, 0), mock.write_count);
}

test "stream_set writes wake or standby and waits for the transition" {
    var device = freshDevice();
    try testing.expectEqual(@as(u16, 0), abi.ra8_ov5640_stream_set(&device, true));
    try testing.expectEqual(@as(u8, 0x02), mock.regs[0x3008]);
    try testing.expectEqual(@as(u32, 1), mock.delay_count);
    try testing.expectEqual(@as(u32, 5), mock.delays[0]);

    try testing.expectEqual(@as(u16, 0), abi.ra8_ov5640_stream_set(&device, false));
    try testing.expectEqual(@as(u8, 0x42), mock.regs[0x3008]);
    try testing.expectEqual(@as(u32, 2), mock.delay_count);
}

test "stream_set skips the settle delay when the write faults" {
    var device = freshDevice();
    mock.fail_write_at = 0;
    try testing.expectEqual(nack, abi.ra8_ov5640_stream_set(&device, true));
    try testing.expectEqual(@as(u32, 0), mock.delay_count);
}

test "a hand-built device with no read callback answers null_ptr" {
    mock.reset();
    var device: abi.Device = .{
        .bus = .{ .write_reg = &mockWrite, .delay_ms = &mockDelay },
        .initialized = true,
    };
    var value: u8 = 0;
    log_count = 0;
    try testing.expectEqual(null_ptr, abi.ra8_ov5640_read_reg(&device, 0x300A, &value));
    try testing.expectEqual(@as(u32, 1), log_count);
}

test "a hand-built device with no write callback answers null_ptr" {
    mock.reset();
    var device: abi.Device = .{
        .bus = .{ .read_reg = &mockRead, .delay_ms = &mockDelay },
        .initialized = true,
    };
    log_count = 0;
    try testing.expectEqual(null_ptr, abi.ra8_ov5640_write_reg(&device, 0x300A, 0x01));
    try testing.expectEqual(@as(u32, 1), log_count);
}

test "a hand-built device with no delay callback still streams" {
    mock.reset();
    var device: abi.Device = .{
        .bus = .{ .read_reg = &mockRead, .write_reg = &mockWrite },
        .address = 0x3C,
        .initialized = true,
    };
    try testing.expectEqual(@as(u16, 0), abi.ra8_ov5640_stream_set(&device, true));
    try testing.expectEqual(@as(u32, 0), mock.delay_count);
}

test "exported layouts match the caller-owned c structs" {
    const word = @sizeOf(usize);
    try testing.expectEqual(word * 4, @sizeOf(abi.Bus));
    try testing.expectEqual(word * 5, @sizeOf(abi.Device));
    try testing.expectEqual(word * 4, @offsetOf(abi.Device, "address"));
    try testing.expectEqual(@as(usize, 16), @sizeOf(abi.JpegStatus));
}
