//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Zig-native tests for the C ABI membrane. A canned-response mock transport
//! stands in for the I2C / SPI bus exactly as the host C suite's mock does,
//! and a test-only C `ra8_log_emit_error` sink lets each guard assert both the
//! `ra8_err_t` code and the message the C emitted.

const std = @import("std");
const testing = std.testing;
const abi = @import("abi");

const ok: u16 = 0;
const err_invalid_arg: u16 = 0x103;
const err_not_initialized: u16 = 0x10F;
const err_nack: u16 = 0x407;
const err_null_ptr: u16 = 0x504;

// ---------------------------------------------------------------------------
// Log sink: the real build links libs/ra8_core/src/ra8_log.c; here the test
// binary substitutes its own recorder at the same link-time seam.
// ---------------------------------------------------------------------------

extern fn ra8_test_fixture_reset() void;
extern fn ra8_test_log_count() u32;
extern fn ra8_test_log_last() [*:0]const u8;

fn lastLogIs(expected: []const u8) !void {
    try testing.expectEqualStrings(expected, std.mem.span(ra8_test_log_last()));
}

// ---------------------------------------------------------------------------
// Mock transport over a flat register file.
// ---------------------------------------------------------------------------

const Mock = struct {
    regs: [256]u8 = @splat(0),
    read_status: u16 = ok,
    write_status: u16 = ok,
    fail_write_after: u32 = 0xFFFF_FFFF,
    reads: u32 = 0,
    writes: u32 = 0,
    last_read_reg: u8 = 0,
    last_read_len: u32 = 0,
    last_write_reg: u8 = 0,
    last_write_value: u8 = 0,
};

var mock: Mock = .{};

fn mockRead(ctx: ?*anyopaque, reg: u8, buf: ?[*]u8, len: u32) callconv(.c) u16 {
    _ = ctx;
    mock.reads += 1;
    mock.last_read_reg = reg;
    mock.last_read_len = len;
    if (mock.read_status != ok) {
        return mock.read_status;
    }
    const out = buf orelse return err_null_ptr;
    var i: u32 = 0;
    while (i < len) : (i += 1) {
        out[i] = mock.regs[(@as(u32, reg) + i) & 0xFF];
    }
    return ok;
}

fn mockWrite(ctx: ?*anyopaque, reg: u8, buf: ?[*]const u8, len: u32) callconv(.c) u16 {
    _ = ctx;
    mock.writes += 1;
    mock.last_write_reg = reg;
    const source = buf orelse return err_null_ptr;
    mock.last_write_value = source[0];
    if (mock.write_status != ok and mock.writes > mock.fail_write_after) {
        return mock.write_status;
    }
    var i: u32 = 0;
    while (i < len) : (i += 1) {
        mock.regs[(@as(u32, reg) + i) & 0xFF] = source[i];
    }
    return ok;
}

fn freshBus() abi.Bus {
    mock = .{};
    ra8_test_fixture_reset();
    return .{ .read_regs = &mockRead, .write_regs = &mockWrite, .ctx = null };
}

/// A device bound to the mock, as `ra8_lsm6dso_init` leaves it.
fn boundDevice(dev: *abi.Device) !void {
    const bus = freshBus();
    try testing.expectEqual(ok, abi.ra8_lsm6dso_init(dev, &bus));
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

test "init binds the transport and seeds the reset defaults" {
    var dev: abi.Device = .{};
    try boundDevice(&dev);
    try testing.expect(dev.initialized);
    try testing.expectEqual(@as(u8, 0x00), dev.accel_fs_code);
    try testing.expectEqual(@as(u8, 0x01), dev.gyro_fs_code);
    try testing.expectEqual(@as(u8, 0x00), dev.odr_code);
    try testing.expectEqual(@as(u32, 0), ra8_test_log_count());
}

test "init rejects a NULL out_dev first" {
    const bus = freshBus();
    try testing.expectEqual(err_null_ptr, abi.ra8_lsm6dso_init(null, &bus));
    try lastLogIs("init: out_dev");
}

test "init rejects a NULL bus" {
    var dev: abi.Device = .{};
    _ = freshBus();
    try testing.expectEqual(err_null_ptr, abi.ra8_lsm6dso_init(&dev, null));
    try lastLogIs("init: bus");
    try testing.expect(!dev.initialized);
}

test "init rejects a bus with no read callback" {
    var dev: abi.Device = .{};
    var bus = freshBus();
    bus.read_regs = null;
    try testing.expectEqual(err_null_ptr, abi.ra8_lsm6dso_init(&dev, &bus));
    try lastLogIs("init: bus.read_regs");
    try testing.expect(!dev.initialized);
}

test "init rejects a bus with no write callback" {
    var dev: abi.Device = .{};
    var bus = freshBus();
    bus.write_regs = null;
    try testing.expectEqual(err_null_ptr, abi.ra8_lsm6dso_init(&dev, &bus));
    try lastLogIs("init: bus.write_regs");
    try testing.expect(!dev.initialized);
}

// ---------------------------------------------------------------------------
// Identification
// ---------------------------------------------------------------------------

test "who_am_i returns the part's 0x6C signature" {
    var dev: abi.Device = .{};
    try boundDevice(&dev);
    mock.regs[0x0F] = 0x6C;
    var id: u8 = 0;
    try testing.expectEqual(ok, abi.ra8_lsm6dso_who_am_i(&dev, &id));
    try testing.expectEqual(@as(u8, 0x6C), id);
    try testing.expectEqual(@as(u8, 0x0F), mock.last_read_reg);
    try testing.expectEqual(@as(u32, 1), mock.last_read_len);
}

test "who_am_i hands back a wrong signature without judging it" {
    var dev: abi.Device = .{};
    try boundDevice(&dev);
    mock.regs[0x0F] = 0xFF;
    var id: u8 = 0;
    try testing.expectEqual(ok, abi.ra8_lsm6dso_who_am_i(&dev, &id));
    try testing.expectEqual(@as(u8, 0xFF), id);
}

test "who_am_i rejects a NULL device before the output pointer" {
    _ = freshBus();
    try testing.expectEqual(err_null_ptr, abi.ra8_lsm6dso_who_am_i(null, null));
    try lastLogIs("who_am_i: dev");
}

test "who_am_i rejects a NULL output pointer" {
    var dev: abi.Device = .{};
    try boundDevice(&dev);
    try testing.expectEqual(err_null_ptr, abi.ra8_lsm6dso_who_am_i(&dev, null));
    try lastLogIs("who_am_i: out_id");
}

test "who_am_i refuses an uninitialized device after the NULL guards" {
    var dev: abi.Device = .{};
    _ = freshBus();
    var id: u8 = 0;
    try testing.expectEqual(err_not_initialized, abi.ra8_lsm6dso_who_am_i(&dev, &id));
    try lastLogIs("who_am_i: not initialized");
}

test "who_am_i forwards a transport NAK unchanged" {
    var dev: abi.Device = .{};
    try boundDevice(&dev);
    mock.read_status = err_nack;
    var id: u8 = 0;
    try testing.expectEqual(err_nack, abi.ra8_lsm6dso_who_am_i(&dev, &id));
}

// ---------------------------------------------------------------------------
// Configuration: accelerometer full scale
// ---------------------------------------------------------------------------

test "set_accel_range programs FS_XL and leaves the ODR nibble alone" {
    var dev: abi.Device = .{};
    try boundDevice(&dev);
    mock.regs[0x10] = 0x40; // ODR = 4 pre-seeded
    try testing.expectEqual(ok, abi.ra8_lsm6dso_set_accel_range(&dev, 0x02));
    try testing.expectEqual(@as(u8, 0x48), mock.regs[0x10]);
    try testing.expectEqual(@as(u8, 0x10), mock.last_write_reg);
    try testing.expectEqual(@as(u8, 0x02), dev.accel_fs_code);
}

test "set_accel_range accepts the whole 2g..8g range" {
    var dev: abi.Device = .{};
    var fs: u8 = 0;
    while (fs <= 0x03) : (fs += 1) {
        try boundDevice(&dev);
        try testing.expectEqual(ok, abi.ra8_lsm6dso_set_accel_range(&dev, fs));
        try testing.expectEqual(fs, dev.accel_fs_code);
        try testing.expectEqual(@as(u8, fs << 2), mock.regs[0x10]);
    }
}

test "set_accel_range rejects a code past 8g and leaves the cache alone" {
    var dev: abi.Device = .{};
    try boundDevice(&dev);
    try testing.expectEqual(err_invalid_arg, abi.ra8_lsm6dso_set_accel_range(&dev, 0x04));
    try lastLogIs("Range check failed");
    try testing.expectEqual(@as(u8, 0x00), dev.accel_fs_code);
    try testing.expectEqual(@as(u32, 0), mock.writes);
}

test "set_accel_range rejects a NULL device" {
    _ = freshBus();
    try testing.expectEqual(err_null_ptr, abi.ra8_lsm6dso_set_accel_range(null, 0x01));
    try lastLogIs("set_accel_range: dev");
}

test "set_accel_range checks initialization before the range" {
    var dev: abi.Device = .{};
    _ = freshBus();
    try testing.expectEqual(err_not_initialized, abi.ra8_lsm6dso_set_accel_range(&dev, 0xFF));
    try lastLogIs("set_accel_range: not initialized");
}

test "set_accel_range propagates a read fault and writes nothing" {
    var dev: abi.Device = .{};
    try boundDevice(&dev);
    mock.read_status = err_nack;
    try testing.expectEqual(err_nack, abi.ra8_lsm6dso_set_accel_range(&dev, 0x03));
    try testing.expectEqual(@as(u32, 0), mock.writes);
    try testing.expectEqual(@as(u8, 0x00), dev.accel_fs_code);
}

test "set_accel_range propagates a write fault and leaves the cache stale" {
    var dev: abi.Device = .{};
    try boundDevice(&dev);
    mock.write_status = err_nack;
    mock.fail_write_after = 0;
    try testing.expectEqual(err_nack, abi.ra8_lsm6dso_set_accel_range(&dev, 0x03));
    try testing.expectEqual(@as(u8, 0x00), dev.accel_fs_code);
}

// ---------------------------------------------------------------------------
// Configuration: gyroscope full scale
// ---------------------------------------------------------------------------

test "set_gyro_range selects the narrow 125 dps scale through FS_125" {
    var dev: abi.Device = .{};
    try boundDevice(&dev);
    mock.regs[0x11] = 0x50; // ODR = 5 pre-seeded
    try testing.expectEqual(ok, abi.ra8_lsm6dso_set_gyro_range(&dev, 0x00));
    try testing.expectEqual(@as(u8, 0x52), mock.regs[0x11]);
    try testing.expectEqual(@as(u8, 0x00), dev.gyro_fs_code);
}

test "set_gyro_range programs FS_G for 2000 dps and clears FS_125" {
    var dev: abi.Device = .{};
    try boundDevice(&dev);
    mock.regs[0x11] = 0x02; // FS_125 currently set
    try testing.expectEqual(ok, abi.ra8_lsm6dso_set_gyro_range(&dev, 0x04));
    try testing.expectEqual(@as(u8, 0x0C), mock.regs[0x11]);
    try testing.expectEqual(@as(u8, 0x04), dev.gyro_fs_code);
}

test "set_gyro_range rejects a code past 2000 dps" {
    var dev: abi.Device = .{};
    try boundDevice(&dev);
    try testing.expectEqual(err_invalid_arg, abi.ra8_lsm6dso_set_gyro_range(&dev, 0x05));
    try lastLogIs("Range check failed");
    try testing.expectEqual(@as(u32, 0), mock.writes);
}

test "set_gyro_range rejects a NULL device" {
    _ = freshBus();
    try testing.expectEqual(err_null_ptr, abi.ra8_lsm6dso_set_gyro_range(null, 0x01));
    try lastLogIs("set_gyro_range: dev");
}

test "set_gyro_range refuses an uninitialized device" {
    var dev: abi.Device = .{};
    _ = freshBus();
    try testing.expectEqual(err_not_initialized, abi.ra8_lsm6dso_set_gyro_range(&dev, 0x01));
    try lastLogIs("set_gyro_range: not initialized");
}

test "set_gyro_range propagates a read fault" {
    var dev: abi.Device = .{};
    try boundDevice(&dev);
    mock.read_status = err_nack;
    try testing.expectEqual(err_nack, abi.ra8_lsm6dso_set_gyro_range(&dev, 0x02));
    try testing.expectEqual(@as(u8, 0x01), dev.gyro_fs_code);
}

// ---------------------------------------------------------------------------
// Configuration: output data rate
// ---------------------------------------------------------------------------

test "set_odr writes the nibble into both control blocks" {
    var dev: abi.Device = .{};
    try boundDevice(&dev);
    mock.regs[0x10] = 0x0C; // FS_XL = 8g
    mock.regs[0x11] = 0x02; // FS_125 set
    try testing.expectEqual(ok, abi.ra8_lsm6dso_set_odr(&dev, 0x04));
    try testing.expectEqual(@as(u8, 0x4C), mock.regs[0x10]);
    try testing.expectEqual(@as(u8, 0x42), mock.regs[0x11]);
    try testing.expectEqual(@as(u8, 0x04), dev.odr_code);
}

test "set_odr powers the part down without touching the FS fields" {
    var dev: abi.Device = .{};
    try boundDevice(&dev);
    mock.regs[0x10] = 0xAC;
    mock.regs[0x11] = 0xAE;
    try testing.expectEqual(ok, abi.ra8_lsm6dso_set_odr(&dev, 0x00));
    try testing.expectEqual(@as(u8, 0x0C), mock.regs[0x10]);
    try testing.expectEqual(@as(u8, 0x0E), mock.regs[0x11]);
}

test "set_odr accepts every rate up to 6.66 kHz" {
    var dev: abi.Device = .{};
    var odr: u8 = 0;
    while (odr <= 0x0A) : (odr += 1) {
        try boundDevice(&dev);
        try testing.expectEqual(ok, abi.ra8_lsm6dso_set_odr(&dev, odr));
        try testing.expectEqual(@as(u8, odr << 4), mock.regs[0x10]);
        try testing.expectEqual(@as(u8, odr << 4), mock.regs[0x11]);
    }
}

test "set_odr rejects a rate past the top code" {
    var dev: abi.Device = .{};
    try boundDevice(&dev);
    try testing.expectEqual(err_invalid_arg, abi.ra8_lsm6dso_set_odr(&dev, 0x0B));
    try lastLogIs("Range check failed");
    try testing.expectEqual(@as(u32, 0), mock.writes);
}

test "set_odr rejects a NULL device" {
    _ = freshBus();
    try testing.expectEqual(err_null_ptr, abi.ra8_lsm6dso_set_odr(null, 0x01));
    try lastLogIs("set_odr: dev");
}

test "set_odr refuses an uninitialized device" {
    var dev: abi.Device = .{};
    _ = freshBus();
    try testing.expectEqual(err_not_initialized, abi.ra8_lsm6dso_set_odr(&dev, 0x01));
    try lastLogIs("set_odr: not initialized");
}

test "set_odr stops at the accel block when its write faults" {
    var dev: abi.Device = .{};
    try boundDevice(&dev);
    mock.write_status = err_nack;
    mock.fail_write_after = 0;
    try testing.expectEqual(err_nack, abi.ra8_lsm6dso_set_odr(&dev, 0x06));
    try testing.expectEqual(@as(u32, 1), mock.writes);
    try testing.expectEqual(@as(u8, 0x00), dev.odr_code);
}

test "set_odr leaves the cache stale when the gyro block write faults" {
    var dev: abi.Device = .{};
    try boundDevice(&dev);
    mock.write_status = err_nack;
    mock.fail_write_after = 1; // let CTRL1_XL through, fail CTRL2_G
    try testing.expectEqual(err_nack, abi.ra8_lsm6dso_set_odr(&dev, 0x06));
    try testing.expectEqual(@as(u8, 0x60), mock.regs[0x10]);
    try testing.expectEqual(@as(u8, 0x00), dev.odr_code);
}

// ---------------------------------------------------------------------------
// Sample reads
// ---------------------------------------------------------------------------

test "read_accel bursts six bytes from OUTX_L_A and combines them" {
    var dev: abi.Device = .{};
    try boundDevice(&dev);
    mock.regs[0x28] = 0x34;
    mock.regs[0x29] = 0x12;
    mock.regs[0x2A] = 0xFF;
    mock.regs[0x2B] = 0xFF;
    mock.regs[0x2C] = 0x00;
    mock.regs[0x2D] = 0x80;
    var sample: abi.Xyz = .{ .x = 0, .y = 0, .z = 0 };
    try testing.expectEqual(ok, abi.ra8_lsm6dso_read_accel(&dev, &sample));
    try testing.expectEqual(@as(i16, 0x1234), sample.x);
    try testing.expectEqual(@as(i16, -1), sample.y);
    try testing.expectEqual(@as(i16, -32768), sample.z);
    try testing.expectEqual(@as(u8, 0x28), mock.last_read_reg);
    try testing.expectEqual(@as(u32, 6), mock.last_read_len);
}

test "read_gyro bursts from OUTX_L_G instead" {
    var dev: abi.Device = .{};
    try boundDevice(&dev);
    mock.regs[0x22] = 0x01;
    mock.regs[0x23] = 0x00;
    var sample: abi.Xyz = .{ .x = 0, .y = 0, .z = 0 };
    try testing.expectEqual(ok, abi.ra8_lsm6dso_read_gyro(&dev, &sample));
    try testing.expectEqual(@as(i16, 1), sample.x);
    try testing.expectEqual(@as(u8, 0x22), mock.last_read_reg);
}

test "read_accel rejects a NULL device before the sample pointer" {
    _ = freshBus();
    try testing.expectEqual(err_null_ptr, abi.ra8_lsm6dso_read_accel(null, null));
    try lastLogIs("read_accel: dev");
}

test "read_accel rejects a NULL sample pointer" {
    var dev: abi.Device = .{};
    try boundDevice(&dev);
    try testing.expectEqual(err_null_ptr, abi.ra8_lsm6dso_read_accel(&dev, null));
    try lastLogIs("read_accel: out");
}

test "read_gyro refuses an uninitialized device" {
    var dev: abi.Device = .{};
    _ = freshBus();
    var sample: abi.Xyz = .{ .x = 0, .y = 0, .z = 0 };
    try testing.expectEqual(err_not_initialized, abi.ra8_lsm6dso_read_gyro(&dev, &sample));
    try lastLogIs("read_gyro: not initialized");
}

test "a bus fault leaves the caller's sample untouched" {
    var dev: abi.Device = .{};
    try boundDevice(&dev);
    mock.read_status = err_nack;
    var sample: abi.Xyz = .{ .x = 7, .y = 8, .z = 9 };
    try testing.expectEqual(err_nack, abi.ra8_lsm6dso_read_accel(&dev, &sample));
    try testing.expectEqual(@as(i16, 7), sample.x);
    try testing.expectEqual(@as(i16, 8), sample.y);
    try testing.expectEqual(@as(i16, 9), sample.z);
}

test "read_temp converts the raw die sample into centi-degrees" {
    var dev: abi.Device = .{};
    try boundDevice(&dev);
    mock.regs[0x20] = 0x00;
    mock.regs[0x21] = 0x01; // raw 256 -> +26.00 C
    var centi: i32 = 0;
    try testing.expectEqual(ok, abi.ra8_lsm6dso_read_temp(&dev, &centi));
    try testing.expectEqual(@as(i32, 2600), centi);
    try testing.expectEqual(@as(u8, 0x20), mock.last_read_reg);
    try testing.expectEqual(@as(u32, 2), mock.last_read_len);
}

test "read_temp handles a sample below the zero offset" {
    var dev: abi.Device = .{};
    try boundDevice(&dev);
    mock.regs[0x20] = 0x00;
    mock.regs[0x21] = 0xFF; // raw -256 -> +24.00 C
    var centi: i32 = 0;
    try testing.expectEqual(ok, abi.ra8_lsm6dso_read_temp(&dev, &centi));
    try testing.expectEqual(@as(i32, 2400), centi);
}

test "read_temp rejects a NULL output pointer" {
    var dev: abi.Device = .{};
    try boundDevice(&dev);
    try testing.expectEqual(err_null_ptr, abi.ra8_lsm6dso_read_temp(&dev, null));
    try lastLogIs("read_temp: out_centi_c");
}

test "read_temp forwards a transport fault" {
    var dev: abi.Device = .{};
    try boundDevice(&dev);
    mock.read_status = err_nack;
    var centi: i32 = 123;
    try testing.expectEqual(err_nack, abi.ra8_lsm6dso_read_temp(&dev, &centi));
    try testing.expectEqual(@as(i32, 123), centi);
}

// ---------------------------------------------------------------------------
// FIFO drain
// ---------------------------------------------------------------------------

test "read_fifo drains the live depth and reports the word count" {
    var dev: abi.Device = .{};
    try boundDevice(&dev);
    mock.regs[0x3A] = 0x03; // three words live
    mock.regs[0x3B] = 0x00;
    mock.regs[0x78] = 0xAA;
    var buffer: [64]u8 = @splat(0);
    var words: u32 = 0xFFFF;
    try testing.expectEqual(ok, abi.ra8_lsm6dso_read_xl_gyro_fifo(&dev, &buffer, 8, &words));
    try testing.expectEqual(@as(u32, 3), words);
    try testing.expectEqual(@as(u8, 0x78), mock.last_read_reg);
    try testing.expectEqual(@as(u32, 21), mock.last_read_len); // 3 words * 7 bytes
    try testing.expectEqual(@as(u8, 0xAA), buffer[0]);
}

test "read_fifo clamps a deep FIFO to the caller's cap" {
    var dev: abi.Device = .{};
    try boundDevice(&dev);
    mock.regs[0x3A] = 0xFF;
    mock.regs[0x3B] = 0x03; // 1023 words live
    var buffer: [64]u8 = @splat(0);
    var words: u32 = 0;
    try testing.expectEqual(ok, abi.ra8_lsm6dso_read_xl_gyro_fifo(&dev, &buffer, 4, &words));
    try testing.expectEqual(@as(u32, 4), words);
    try testing.expectEqual(@as(u32, 28), mock.last_read_len);
}

test "read_fifo on an empty FIFO succeeds with zero words and no data read" {
    var dev: abi.Device = .{};
    try boundDevice(&dev);
    var buffer: [64]u8 = @splat(0);
    var words: u32 = 9;
    try testing.expectEqual(ok, abi.ra8_lsm6dso_read_xl_gyro_fifo(&dev, &buffer, 8, &words));
    try testing.expectEqual(@as(u32, 0), words);
    try testing.expectEqual(@as(u8, 0x3A), mock.last_read_reg); // status only
}

test "read_fifo zeroes the caller's count before any guard runs" {
    _ = freshBus();
    var words: u32 = 42;
    try testing.expectEqual(
        err_null_ptr,
        abi.ra8_lsm6dso_read_xl_gyro_fifo(null, null, 8, &words),
    );
    try testing.expectEqual(@as(u32, 0), words);
    try lastLogIs("read_fifo: dev");
}

test "read_fifo rejects a NULL buffer after the device" {
    var dev: abi.Device = .{};
    try boundDevice(&dev);
    var words: u32 = 0;
    try testing.expectEqual(
        err_null_ptr,
        abi.ra8_lsm6dso_read_xl_gyro_fifo(&dev, null, 8, &words),
    );
    try lastLogIs("read_fifo: out_buf");
}

test "read_fifo rejects a NULL count pointer" {
    var dev: abi.Device = .{};
    try boundDevice(&dev);
    var buffer: [8]u8 = @splat(0);
    try testing.expectEqual(
        err_null_ptr,
        abi.ra8_lsm6dso_read_xl_gyro_fifo(&dev, &buffer, 8, null),
    );
    try lastLogIs("read_fifo: out_words");
}

test "read_fifo refuses an uninitialized device after the NULL guards" {
    var dev: abi.Device = .{};
    _ = freshBus();
    var buffer: [8]u8 = @splat(0);
    var words: u32 = 0;
    try testing.expectEqual(
        err_not_initialized,
        abi.ra8_lsm6dso_read_xl_gyro_fifo(&dev, &buffer, 8, &words),
    );
    try lastLogIs("read_fifo: not initialized");
}

test "read_fifo rejects a zero word cap last" {
    var dev: abi.Device = .{};
    try boundDevice(&dev);
    var buffer: [8]u8 = @splat(0);
    var words: u32 = 0;
    try testing.expectEqual(
        err_invalid_arg,
        abi.ra8_lsm6dso_read_xl_gyro_fifo(&dev, &buffer, 0, &words),
    );
    try lastLogIs("read_fifo: max_words is zero");
    try testing.expectEqual(@as(u32, 0), mock.reads);
}

test "read_fifo propagates a status-read fault with the count still zero" {
    var dev: abi.Device = .{};
    try boundDevice(&dev);
    mock.read_status = err_nack;
    var buffer: [8]u8 = @splat(0);
    var words: u32 = 5;
    try testing.expectEqual(
        err_nack,
        abi.ra8_lsm6dso_read_xl_gyro_fifo(&dev, &buffer, 1, &words),
    );
    try testing.expectEqual(@as(u32, 0), words);
}

// ---------------------------------------------------------------------------
// Hardening beyond the C: a hand-built device with no transport
// ---------------------------------------------------------------------------

test "a device with a NULL read seam answers null_ptr instead of trapping" {
    _ = freshBus();
    var dev: abi.Device = .{};
    dev.initialized = true;
    var id: u8 = 0;
    try testing.expectEqual(err_null_ptr, abi.ra8_lsm6dso_who_am_i(&dev, &id));
    try lastLogIs("bus: read_regs");
}

test "a device with a NULL write seam answers null_ptr instead of trapping" {
    _ = freshBus();
    var dev: abi.Device = .{};
    dev.initialized = true;
    dev.bus.read_regs = &mockRead;
    try testing.expectEqual(err_null_ptr, abi.ra8_lsm6dso_set_accel_range(&dev, 0x01));
    try lastLogIs("bus: write_regs");
}
