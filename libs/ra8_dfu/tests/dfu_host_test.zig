//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The DFU host sequence driven against a scripted device.
//!
//! The steps are generic over their HAL, so `FakeDevice` below stands in for
//! the controller: it answers control transfers the way a DFU device would,
//! records the wire traffic, and can be told to fail or short any step. That
//! is the only way this driver is testable at all without a board, since
//! every real call it makes reaches a USB register.

const std = @import("std");
const testing = std.testing;

const Err = @import("err").Err;
const attach = @import("attach");
const control = @import("control");
const download = @import("download");
const hal = @import("hal");
const proto = @import("proto");
const session = @import("session");
const status = @import("status");
const tune = @import("tune");
const verify = @import("verify");

const block_bytes = proto.Session.transfer_size;

/// One control transfer as the device saw it.
const Exchange = struct {
    bm_request_type: u8,
    b_request: u8,
    w_value: u16,
    w_length: u16,
};

/// A scripted DFU device plus the controller around it.
///
/// Module-level rather than an instance, because the step functions take a
/// HAL *type*: that is what keeps the production path free of a vtable.
const FakeDevice = struct {
    /// What the device holds after the download phase.
    var storage: [64 * block_bytes]u8 = @splat(0);
    var storage_len: usize = 0;

    /// Wire log, in order.
    var log: [512]Exchange = undefined;
    var log_len: usize = 0;

    /// Elapsed milliseconds, advanced only by `delayMs`.
    var clock: u32 = 0;
    /// Line state returned by the attach spin.
    var line: u16 = 1;

    // -- scripted failures -------------------------------------------------
    /// Controller init result.
    var init_err: Err = .ok;
    /// Descriptor reads that fail before one succeeds.
    var desc_failures: u32 = 0;
    /// Truncate the descriptor read to this many bytes when set.
    var desc_short_to: ?u16 = null;
    /// Fail the nth DFU_DNLOAD (0-based) with this code.
    var dnload_fail_at: ?struct { block: u16, err: Err } = null;
    /// Corrupt one byte of this block before it is uploaded back.
    var corrupt_block: ?u16 = null;
    /// Return a short payload for this upload block.
    var upload_short_at: ?u16 = null;
    /// bState the device reports; the driver polls until it matches.
    var state: u8 = proto.State.dfu_dnload_idle;
    /// GETSTATUS polls that report the wrong state before the right one.
    var state_stalls: u32 = 0;
    var init_calls: u32 = 0;
    var deinit_calls: u32 = 0;
    var bus_resets: u32 = 0;

    fn reset() void {
        storage = @splat(0);
        storage_len = 0;
        log_len = 0;
        clock = 0;
        line = 1;
        init_err = .ok;
        desc_failures = 0;
        desc_short_to = null;
        dnload_fail_at = null;
        corrupt_block = null;
        upload_short_at = null;
        state = proto.State.dfu_dnload_idle;
        state_stalls = 0;
        init_calls = 0;
        deinit_calls = 0;
        bus_resets = 0;
    }

    fn record(setup: *const hal.Setup) void {
        if (log_len < log.len) {
            log[log_len] = .{
                .bm_request_type = setup.bm_request_type,
                .b_request = setup.b_request,
                .w_value = setup.w_value,
                .w_length = setup.w_length,
            };
            log_len += 1;
        }
    }

    fn countOf(bm: u8, request: u8) u32 {
        var n: u32 = 0;
        for (log[0..log_len]) |entry| {
            if (entry.bm_request_type == bm and entry.b_request == request) n += 1;
        }
        return n;
    }

    // -- the HAL surface ---------------------------------------------------
    pub fn init(_: hal.Speed) Err {
        init_calls += 1;
        return init_err;
    }

    pub fn deinit(_: hal.Speed) Err {
        deinit_calls += 1;
        return .ok;
    }

    pub fn busReset(_: hal.Speed, assert_reset: bool) Err {
        if (assert_reset) bus_resets += 1;
        return .ok;
    }

    pub fn setUact(_: hal.Speed, _: bool) Err {
        return .ok;
    }

    pub fn setTarget(_: hal.Speed, _: u8) Err {
        return .ok;
    }

    pub fn lineState(_: hal.Speed) u16 {
        return line;
    }

    pub fn timeMs() u32 {
        return clock;
    }

    pub fn delayMs(ms: u32) void {
        clock +%= ms;
    }

    pub fn controlXfer(
        _: hal.Speed,
        setup: *const hal.Setup,
        data: ?[]u8,
        data_len: u16,
        out_received: ?*u16,
    ) Err {
        record(setup);
        const is_in = (setup.bm_request_type & 0x80) != 0;
        const is_class = (setup.bm_request_type & 0x20) != 0;

        if (!is_class and is_in and setup.b_request == proto.Request.get_descriptor) {
            if (desc_failures > 0) {
                desc_failures -= 1;
                return .hw_error;
            }
            const buffer = data.?;
            @memset(buffer[0..data_len], 0);
            // Only the bytes the driver reads need to be real.
            buffer[proto.DeviceDescriptor.id_product_offset] = 0x34;
            buffer[proto.DeviceDescriptor.id_product_offset + 1] = 0x12;
            if (out_received) |received| {
                received.* = desc_short_to orelse data_len;
            }
            return .ok;
        }

        if (is_class and is_in and setup.b_request == proto.Request.dfu_getstatus) {
            const buffer = data.?;
            @memset(buffer[0..data_len], 0);
            if (state_stalls > 0) {
                state_stalls -= 1;
                buffer[proto.GetStatus.state_offset] = 0xEE;
            } else {
                buffer[proto.GetStatus.state_offset] = state;
            }
            if (out_received) |received| received.* = data_len;
            return .ok;
        }

        if (is_class and !is_in and setup.b_request == proto.Request.dfu_dnload) {
            if (dnload_fail_at) |fail| {
                if (fail.block == setup.w_value and setup.w_length != 0) return fail.err;
            }
            if (setup.w_length == 0) {
                // End-of-download: commit, and leave the download state.
                state = proto.State.dfu_idle;
                return .ok;
            }
            const offset = @as(usize, setup.w_value) * block_bytes;
            @memcpy(storage[offset..][0..setup.w_length], data.?[0..setup.w_length]);
            storage_len = @max(storage_len, offset + setup.w_length);
            state = proto.State.dfu_dnload_idle;
            return .ok;
        }

        if (is_class and !is_in and setup.b_request == proto.Request.dfu_abort) {
            state = proto.State.dfu_idle;
            return .ok;
        }

        if (is_class and is_in and setup.b_request == proto.Request.dfu_upload) {
            const buffer = data.?;
            const offset = @as(usize, setup.w_value) * block_bytes;
            @memcpy(buffer[0..block_bytes], storage[offset..][0..block_bytes]);
            if (corrupt_block) |block| {
                if (block == setup.w_value) buffer[0] ^= 0xFF;
            }
            if (out_received) |received| {
                const short = upload_short_at orelse 0xFFFF;
                received.* = if (short == setup.w_value) block_bytes - 1 else block_bytes;
            }
            return .ok;
        }

        // SET_ADDRESS / SET_CONFIGURATION and anything else: accept.
        if (out_received) |received| received.* = 0;
        return .ok;
    }
};

fn rampImage(comptime blocks: usize) [blocks * block_bytes]u8 {
    var image: [blocks * block_bytes]u8 = undefined;
    for (&image, 0..) |*byte, index| byte.* = @truncate(index *% 31 +% 7);
    return image;
}

test "round trip downloads every block and verifies them all" {
    FakeDevice.reset();
    const image = rampImage(4);
    var report = session.Report{};
    const err = session.drive(FakeDevice, .hs, &image, .round_trip, &report);

    try testing.expectEqual(Err.ok, err);
    try testing.expectEqual(@as(u32, 0x1234), report.product_id);
    try testing.expectEqual(@as(u32, 4), report.blocks_ok);
    try testing.expectEqual(@as(?u32, null), report.mismatch);
    try testing.expectEqual(@as(u32, 1), FakeDevice.init_calls);
    try testing.expectEqual(@as(u32, 0), FakeDevice.deinit_calls);
    try testing.expectEqualSlices(u8, &image, FakeDevice.storage[0..image.len]);
}

test "round trip sends dnload then abort then upload, in that order" {
    FakeDevice.reset();
    const image = rampImage(2);
    var report = session.Report{};
    _ = session.drive(FakeDevice, .fs, &image, .round_trip, &report);

    try testing.expectEqual(@as(u32, 2), FakeDevice.countOf(proto.Bm.class_if_out, proto.Request.dfu_dnload));
    try testing.expectEqual(@as(u32, 1), FakeDevice.countOf(proto.Bm.class_if_out, proto.Request.dfu_abort));
    try testing.expectEqual(@as(u32, 2), FakeDevice.countOf(proto.Bm.class_if_in, proto.Request.dfu_upload));
}

test "program ends with a zero length dnload and never uploads" {
    FakeDevice.reset();
    const image = rampImage(3);
    var report = session.Report{};
    const err = session.drive(FakeDevice, .hs, &image, .program, &report);

    try testing.expectEqual(Err.ok, err);
    try testing.expectEqual(@as(u32, 0), report.blocks_ok);
    try testing.expectEqual(@as(u32, 0), FakeDevice.countOf(proto.Bm.class_if_in, proto.Request.dfu_upload));
    try testing.expectEqual(@as(u32, 0), FakeDevice.countOf(proto.Bm.class_if_out, proto.Request.dfu_abort));
    // Four DFU_DNLOADs: three blocks plus the end-of-download.
    try testing.expectEqual(@as(u32, 4), FakeDevice.countOf(proto.Bm.class_if_out, proto.Request.dfu_dnload));
    const last = FakeDevice.log[FakeDevice.log_len - 1];
    try testing.expectEqual(@as(u16, 0), last.w_length);
    try testing.expectEqual(@as(u16, 3), last.w_value);
}

test "a corrupted block is reported by index and stops the verify" {
    FakeDevice.reset();
    FakeDevice.corrupt_block = 2;
    const image = rampImage(4);
    var report = session.Report{};
    const err = session.drive(FakeDevice, .hs, &image, .round_trip, &report);

    try testing.expectEqual(Err.invalid_state, err);
    try testing.expectEqual(@as(?u32, 2), report.mismatch);
    try testing.expectEqual(@as(u32, 2), report.blocks_ok);
    try testing.expectEqual(@as(u32, 1), FakeDevice.deinit_calls);
}

test "a short upload block is an invalid size, not a mismatch" {
    FakeDevice.reset();
    FakeDevice.upload_short_at = 1;
    const image = rampImage(3);
    var report = session.Report{};
    const err = session.drive(FakeDevice, .hs, &image, .round_trip, &report);

    try testing.expectEqual(Err.invalid_size, err);
    try testing.expectEqual(@as(?u32, 1), report.mismatch);
    try testing.expectEqual(@as(u32, 1), report.blocks_ok);
}

test "a failing dnload aborts the sequence and tears the controller down" {
    FakeDevice.reset();
    FakeDevice.dnload_fail_at = .{ .block = 1, .err = .hw_error };
    const image = rampImage(4);
    var report = session.Report{};
    const err = session.drive(FakeDevice, .hs, &image, .round_trip, &report);

    try testing.expectEqual(Err.hw_error, err);
    try testing.expectEqual(@as(u32, 0), FakeDevice.countOf(proto.Bm.class_if_in, proto.Request.dfu_upload));
    try testing.expectEqual(@as(u32, 1), FakeDevice.deinit_calls);
}

test "a failed controller init is returned before any wire traffic" {
    FakeDevice.reset();
    FakeDevice.init_err = .hw_error;
    const image = rampImage(1);
    var report = session.Report{};
    const err = session.drive(FakeDevice, .hs, &image, .round_trip, &report);

    try testing.expectEqual(Err.hw_error, err);
    try testing.expectEqual(@as(usize, 0), FakeDevice.log_len);
    try testing.expectEqual(@as(u32, 0), FakeDevice.deinit_calls);
}

test "enumeration retries the reset and succeeds on a later attempt" {
    FakeDevice.reset();
    FakeDevice.desc_failures = 3;
    var desc: [proto.DeviceDescriptor.len]u8 = @splat(0);
    const err = attach.hunt(FakeDevice, .hs, &desc);

    try testing.expectEqual(Err.ok, err);
    try testing.expectEqual(@as(u32, 4), FakeDevice.bus_resets);
    try testing.expectEqual(@as(u32, 0x1234), control.productId(&desc));
}

test "enumeration gives up after every attempt fails" {
    FakeDevice.reset();
    FakeDevice.desc_failures = 1000;
    var desc: [proto.DeviceDescriptor.len]u8 = @splat(0);
    const err = attach.hunt(FakeDevice, .hs, &desc);

    try testing.expectEqual(Err.hw_error, err);
    try testing.expectEqual(@as(u32, tune.Retry.enum_tries), FakeDevice.bus_resets);
}

test "a short descriptor read is a hardware error" {
    FakeDevice.reset();
    FakeDevice.desc_short_to = proto.DeviceDescriptor.len - 1;
    var desc: [proto.DeviceDescriptor.len]u8 = @splat(0);
    const err = control.getDeviceDescriptor(FakeDevice, .hs, &desc);

    try testing.expectEqual(Err.hw_error, err);
}

test "the attach spin stops once the millisecond clock passes the timeout" {
    FakeDevice.reset();
    FakeDevice.line = 0;
    FakeDevice.desc_failures = 1000;
    var desc: [proto.DeviceDescriptor.len]u8 = @splat(0);
    // The fake clock only advances on delayMs, and awaitLine's own
    // vbus settle is enough to put it past the attach timeout, so the spin
    // must exit on the clock rather than run to the iteration cap.
    FakeDevice.clock = tune.Delay.attach_timeout_ms;
    _ = attach.hunt(FakeDevice, .hs, &desc);
    try testing.expectEqual(@as(u32, tune.Retry.enum_tries), FakeDevice.bus_resets);
}

test "a state wait polls until the device agrees" {
    FakeDevice.reset();
    FakeDevice.state = proto.State.dfu_idle;
    FakeDevice.state_stalls = 5;
    const err = status.waitFor(FakeDevice, .hs, proto.State.dfu_idle);

    try testing.expectEqual(Err.ok, err);
    try testing.expectEqual(@as(u32, 6), FakeDevice.countOf(proto.Bm.class_if_in, proto.Request.dfu_getstatus));
}

test "a state wait that never agrees times out after the poll budget" {
    FakeDevice.reset();
    FakeDevice.state = 0x77;
    const err = status.waitFor(FakeDevice, .hs, proto.State.dfu_idle);

    try testing.expectEqual(Err.hw_timeout, err);
    try testing.expectEqual(tune.Retry.status_tries, FakeDevice.countOf(proto.Bm.class_if_in, proto.Request.dfu_getstatus));
}

test "block count is whole blocks only" {
    try testing.expectEqual(@as(u16, 0), download.blockCount(0));
    try testing.expectEqual(@as(u16, 1), download.blockCount(block_bytes));
    try testing.expectEqual(@as(u16, 1), download.blockCount(block_bytes + 1));
    try testing.expectEqual(@as(u16, 16), download.blockCount(16 * block_bytes));
}

test "the setup packets carry the chapter 9 and class request bytes" {
    FakeDevice.reset();
    const image = rampImage(1);
    var report = session.Report{};
    _ = session.drive(FakeDevice, .hs, &image, .round_trip, &report);

    // GET_DESCRIPTOR(DEVICE) asks for type 0x01 in the high byte.
    const first = FakeDevice.log[0];
    try testing.expectEqual(proto.Bm.std_dev_in, first.bm_request_type);
    try testing.expectEqual(proto.Request.get_descriptor, first.b_request);
    try testing.expectEqual(@as(u16, 0x0100), first.w_value);
    try testing.expectEqual(proto.DeviceDescriptor.len, first.w_length);

    var saw_set_address = false;
    var saw_set_config = false;
    for (FakeDevice.log[0..FakeDevice.log_len]) |entry| {
        if (entry.bm_request_type == proto.Bm.std_dev_out and
            entry.b_request == proto.Request.set_address)
        {
            saw_set_address = true;
            try testing.expectEqual(@as(u16, proto.Session.device_address), entry.w_value);
        }
        if (entry.bm_request_type == proto.Bm.std_dev_out and
            entry.b_request == proto.Request.set_configuration)
        {
            saw_set_config = true;
            try testing.expectEqual(proto.Session.configuration_value, entry.w_value);
        }
    }
    try testing.expect(saw_set_address);
    try testing.expect(saw_set_config);
}

test "verify counts the matching prefix even when it fails later" {
    FakeDevice.reset();
    const image = rampImage(5);
    // Prime the device's storage without going through the download phase.
    @memcpy(FakeDevice.storage[0..image.len], &image);
    FakeDevice.corrupt_block = 4;
    var outcome = verify.Outcome{};
    const err = verify.uploadAndCompare(FakeDevice, .hs, &image, &outcome);

    try testing.expectEqual(Err.invalid_state, err);
    try testing.expectEqual(@as(u32, 4), outcome.blocks_ok);
    try testing.expectEqual(@as(?u32, 4), outcome.mismatch);
}
