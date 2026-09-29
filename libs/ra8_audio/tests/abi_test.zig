//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for the exported facade symbols and the in-memory replay
//! backend, driven exactly as C callers drive them: through a fake vtable of
//! this file's own making and through `ra8_audio_source_memory_init`.

const std = @import("std");
const memory = @import("audio");
const abi = memory.abi;

const fixture_samples = [_]i32{ 10, 20, 30, 40, 50, 60, 70, 80 };

fn fixture() abi.Frame {
    return .{
        .data = &fixture_samples,
        .bytes = 32,
        .sample_count = 8,
        .sample_rate_hz = 16000,
        .timestamp_ms = 42,
        .channels = 1,
        .valid_bits = 20,
        .format = 1,
    };
}

fn fixtureInfo() abi.Info {
    return .{
        .frame_bytes = 32,
        .samples_per_frame = 8,
        .sample_rate_hz = 16000,
        .channels = 1,
        .valid_bits = 20,
        .format = 1,
    };
}

/// Injectable backend that reaches every facade failure path, the same shape
/// the C suite's `t_audio_fake_t` uses.
const Fake = struct {
    info_status: u16 = 0,
    capture_status: u16 = 0,
    stream_status: u16 = 0,
    stop_status: u16 = 0,
    info: abi.Info = .{},
    frame: abi.Frame = .{},
    alias_buffer: bool = false,
    info_calls: u32 = 0,
    capture_calls: u32 = 0,
    stream_calls: u32 = 0,
    stop_calls: u32 = 0,
    seen_buffer: ?*const abi.Buffer = null,
    seen_ctx: ?*anyopaque = null,

    fn getInfo(ctx: ?*anyopaque, out_info: ?*abi.Info) callconv(.c) u16 {
        const self: *Fake = @ptrCast(@alignCast(ctx.?));
        self.info_calls += 1;
        if (self.info_status != 0) return self.info_status;
        out_info.?.* = self.info;
        return 0;
    }

    fn capture(
        ctx: ?*anyopaque,
        buffer: ?*const abi.Buffer,
        out_frame: ?*abi.Frame,
    ) callconv(.c) u16 {
        const self: *Fake = @ptrCast(@alignCast(ctx.?));
        self.capture_calls += 1;
        if (self.capture_status != 0) return self.capture_status;
        out_frame.?.* = self.frame;
        if (self.alias_buffer) out_frame.?.data = buffer.?.data;
        return 0;
    }

    fn streamStart(
        ctx: ?*anyopaque,
        buffer: ?*const abi.Buffer,
        callback: ?abi.FrameCallback,
        callback_ctx: ?*anyopaque,
    ) callconv(.c) u16 {
        _ = callback;
        const self: *Fake = @ptrCast(@alignCast(ctx.?));
        if (self.stream_status != 0) return self.stream_status;
        self.stream_calls += 1;
        self.seen_buffer = buffer;
        self.seen_ctx = callback_ctx;
        return 0;
    }

    fn stop(ctx: ?*anyopaque) callconv(.c) u16 {
        const self: *Fake = @ptrCast(@alignCast(ctx.?));
        self.stop_calls += 1;
        return self.stop_status;
    }
};

const fake_iface: abi.SourceIface = .{
    .get_info = Fake.getInfo,
    .capture = Fake.capture,
    .stream_start = Fake.streamStart,
    .stop = Fake.stop,
};
const unbound_iface: abi.SourceIface = .{};

fn newFake() Fake {
    return .{ .info = fixtureInfo(), .frame = fixture(), .alias_buffer = true };
}

fn sourceFor(fake: *Fake) abi.Source {
    return .{ .iface = &fake_iface, .ctx = @ptrCast(fake) };
}

fn noopCallback(ctx: ?*anyopaque, frame: ?*const abi.Frame) callconv(.c) void {
    _ = ctx;
    _ = frame;
}

// --- ra8_audio_frame_validate ---------------------------------------------

test "frame_validate rejects an absent descriptor" {
    try std.testing.expectEqual(abi.err.null_ptr, abi.ra8_audio_frame_validate(null));
}

test "frame_validate accepts the fixture and rejects absent payload bytes" {
    var frame = fixture();
    try std.testing.expectEqual(abi.err.ok, abi.ra8_audio_frame_validate(&frame));
    frame.data = null;
    try std.testing.expectEqual(abi.err.null_ptr, abi.ra8_audio_frame_validate(&frame));
}

test "frame_validate answers invalid_arg for scalar metadata faults" {
    var frame = fixture();
    frame.format = 7;
    try std.testing.expectEqual(abi.err.invalid_arg, abi.ra8_audio_frame_validate(&frame));
    frame = fixture();
    frame.sample_rate_hz = 0;
    try std.testing.expectEqual(abi.err.invalid_arg, abi.ra8_audio_frame_validate(&frame));
    frame = fixture();
    frame.valid_bits = 33;
    try std.testing.expectEqual(abi.err.invalid_arg, abi.ra8_audio_frame_validate(&frame));
}

test "frame_validate answers invalid_size for byte coverage faults" {
    var frame = fixture();
    frame.bytes = 28;
    try std.testing.expectEqual(abi.err.invalid_size, abi.ra8_audio_frame_validate(&frame));
    frame = fixture();
    frame.sample_count = 0x20000004;
    try std.testing.expectEqual(abi.err.invalid_size, abi.ra8_audio_frame_validate(&frame));
}

// --- ra8_audio_source_get_info --------------------------------------------

test "get_info rejects absent arguments" {
    var fake = newFake();
    var source = sourceFor(&fake);
    var info: abi.Info = .{};
    try std.testing.expectEqual(abi.err.null_ptr, abi.ra8_audio_source_get_info(null, &info));
    try std.testing.expectEqual(abi.err.null_ptr, abi.ra8_audio_source_get_info(&source, null));
}

test "get_info zeroes its output before reporting an unbound source" {
    var source = abi.Source{};
    var info = fixtureInfo();
    try std.testing.expectEqual(abi.err.not_initialized, abi.ra8_audio_source_get_info(&source, &info));
    try std.testing.expectEqual(@as(u32, 0), info.frame_bytes);
    try std.testing.expectEqual(@as(u32, 0), info.sample_rate_hz);
}

test "get_info reports not_initialized for a vtable with no info row" {
    var source = abi.Source{ .iface = &unbound_iface };
    var info: abi.Info = .{};
    try std.testing.expectEqual(abi.err.not_initialized, abi.ra8_audio_source_get_info(&source, &info));
}

test "get_info forwards the backend metadata and its errors" {
    var fake = newFake();
    var source = sourceFor(&fake);
    var info: abi.Info = .{};
    try std.testing.expectEqual(abi.err.ok, abi.ra8_audio_source_get_info(&source, &info));
    try std.testing.expectEqual(@as(u32, 32), info.frame_bytes);
    try std.testing.expectEqual(@as(u32, 16000), info.sample_rate_hz);

    fake.info_status = 0x204;
    try std.testing.expectEqual(@as(u16, 0x204), abi.ra8_audio_source_get_info(&source, &info));
}

// --- ra8_audio_source_capture ---------------------------------------------

test "capture rejects each absent argument in order" {
    var fake = newFake();
    var source = sourceFor(&fake);
    var storage = [_]u8{0} ** 32;
    var buffer = abi.Buffer{ .data = &storage, .capacity = storage.len };
    var frame: abi.Frame = .{};
    try std.testing.expectEqual(abi.err.null_ptr, abi.ra8_audio_source_capture(null, &buffer, &frame));
    try std.testing.expectEqual(abi.err.null_ptr, abi.ra8_audio_source_capture(&source, null, &frame));
    try std.testing.expectEqual(abi.err.null_ptr, abi.ra8_audio_source_capture(&source, &buffer, null));
    try std.testing.expectEqual(@as(u32, 0), fake.capture_calls);
}

test "capture zeroes its output before reporting an unbound source" {
    var source = abi.Source{};
    var storage = [_]u8{0} ** 32;
    const buffer = abi.Buffer{ .data = &storage, .capacity = storage.len };
    var frame = fixture();
    try std.testing.expectEqual(abi.err.not_initialized, abi.ra8_audio_source_capture(&source, &buffer, &frame));
    try std.testing.expectEqual(@as(u32, 0), frame.bytes);
    try std.testing.expect(frame.data == null);
}

test "capture treats a data-less or zero-capacity buffer as absent" {
    var fake = newFake();
    var source = sourceFor(&fake);
    var storage = [_]u8{0} ** 32;
    var frame: abi.Frame = .{};
    const no_data = abi.Buffer{ .data = null, .capacity = 32 };
    try std.testing.expectEqual(abi.err.null_ptr, abi.ra8_audio_source_capture(&source, &no_data, &frame));
    const no_capacity = abi.Buffer{ .data = &storage, .capacity = 0 };
    try std.testing.expectEqual(abi.err.null_ptr, abi.ra8_audio_source_capture(&source, &no_capacity, &frame));
    try std.testing.expectEqual(@as(u32, 0), fake.info_calls);
}

test "capture rejects a vtable with no capture row before touching the buffer" {
    var source = abi.Source{ .iface = &unbound_iface };
    const no_data = abi.Buffer{ .data = null, .capacity = 0 };
    var frame: abi.Frame = .{};
    try std.testing.expectEqual(abi.err.not_initialized, abi.ra8_audio_source_capture(&source, &no_data, &frame));
}

test "capture reports not_initialized for a capture row with no info row" {
    // The hardening: the C dereferenced a NULL get_info here.
    const partial_iface = abi.SourceIface{ .capture = Fake.capture };
    var fake = newFake();
    var source = abi.Source{ .iface = &partial_iface, .ctx = @ptrCast(&fake) };
    var storage = [_]u8{0} ** 32;
    const buffer = abi.Buffer{ .data = &storage, .capacity = storage.len };
    var frame: abi.Frame = .{};
    try std.testing.expectEqual(abi.err.not_initialized, abi.ra8_audio_source_capture(&source, &buffer, &frame));
    try std.testing.expectEqual(@as(u32, 0), fake.capture_calls);
}

test "capture forwards a metadata error without dispatching" {
    var fake = newFake();
    fake.info_status = 0x203;
    var source = sourceFor(&fake);
    var storage = [_]u8{0} ** 32;
    const buffer = abi.Buffer{ .data = &storage, .capacity = storage.len };
    var frame: abi.Frame = .{};
    try std.testing.expectEqual(@as(u16, 0x203), abi.ra8_audio_source_capture(&source, &buffer, &frame));
    try std.testing.expectEqual(@as(u32, 0), fake.capture_calls);
}

test "capture rejects a buffer smaller than one source frame" {
    var fake = newFake();
    var source = sourceFor(&fake);
    var storage = [_]u8{0} ** 32;
    const small = abi.Buffer{ .data = &storage, .capacity = 28 };
    var frame: abi.Frame = .{};
    try std.testing.expectEqual(abi.err.invalid_size, abi.ra8_audio_source_capture(&source, &small, &frame));
    try std.testing.expectEqual(@as(u32, 0), fake.capture_calls);
}

test "capture zeroes its output when the backend fails" {
    var fake = newFake();
    fake.capture_status = 0x204;
    var source = sourceFor(&fake);
    var storage = [_]u8{0} ** 32;
    const buffer = abi.Buffer{ .data = &storage, .capacity = storage.len };
    var frame = fixture();
    try std.testing.expectEqual(@as(u16, 0x204), abi.ra8_audio_source_capture(&source, &buffer, &frame));
    try std.testing.expectEqual(@as(u32, 0), frame.bytes);
}

test "capture rejects a frame that does not alias the caller's buffer" {
    var fake = newFake();
    fake.alias_buffer = false;
    var source = sourceFor(&fake);
    var storage = [_]u8{0} ** 32;
    const buffer = abi.Buffer{ .data = &storage, .capacity = storage.len };
    var frame: abi.Frame = .{};
    try std.testing.expectEqual(abi.err.invalid_state, abi.ra8_audio_source_capture(&source, &buffer, &frame));
    try std.testing.expect(frame.data == null);
}

test "capture revalidates the returned descriptor" {
    var fake = newFake();
    fake.frame.valid_bits = 0;
    var source = sourceFor(&fake);
    var storage = [_]u8{0} ** 32;
    const buffer = abi.Buffer{ .data = &storage, .capacity = storage.len };
    var frame: abi.Frame = .{};
    try std.testing.expectEqual(abi.err.invalid_arg, abi.ra8_audio_source_capture(&source, &buffer, &frame));
    try std.testing.expectEqual(@as(u32, 0), frame.bytes);
}

test "capture rejects a frame that disagrees with the advertised geometry" {
    var fake = newFake();
    fake.info.sample_rate_hz = 8000;
    var source = sourceFor(&fake);
    var storage = [_]u8{0} ** 32;
    const buffer = abi.Buffer{ .data = &storage, .capacity = storage.len };
    var frame: abi.Frame = .{};
    try std.testing.expectEqual(abi.err.invalid_state, abi.ra8_audio_source_capture(&source, &buffer, &frame));
    try std.testing.expectEqual(@as(u32, 0), frame.sample_rate_hz);
}

test "capture publishes a frame aliasing the caller's buffer" {
    var fake = newFake();
    var source = sourceFor(&fake);
    var storage = [_]u8{0} ** 32;
    const buffer = abi.Buffer{ .data = &storage, .capacity = storage.len };
    var frame: abi.Frame = .{};
    try std.testing.expectEqual(abi.err.ok, abi.ra8_audio_source_capture(&source, &buffer, &frame));
    try std.testing.expectEqual(@intFromPtr(&storage), @intFromPtr(frame.data.?));
    try std.testing.expectEqual(@as(u32, 32), frame.bytes);
    try std.testing.expectEqual(@as(u32, 1), fake.capture_calls);
}

// --- ra8_audio_source_stream_start ----------------------------------------

test "stream_start rejects each absent argument in order" {
    var fake = newFake();
    var source = sourceFor(&fake);
    var storage = [_]u8{0} ** 32;
    const good = abi.Buffer{ .data = &storage, .capacity = storage.len };
    const no_data = abi.Buffer{ .data = null, .capacity = 32 };
    try std.testing.expectEqual(
        abi.err.null_ptr,
        abi.ra8_audio_source_stream_start(null, &good, noopCallback, null),
    );
    try std.testing.expectEqual(
        abi.err.null_ptr,
        abi.ra8_audio_source_stream_start(&source, null, noopCallback, null),
    );
    try std.testing.expectEqual(
        abi.err.null_ptr,
        abi.ra8_audio_source_stream_start(&source, &good, null, null),
    );
    try std.testing.expectEqual(
        abi.err.null_ptr,
        abi.ra8_audio_source_stream_start(&source, &no_data, noopCallback, null),
    );
    try std.testing.expectEqual(@as(u32, 0), fake.stream_calls);
}

test "stream_start reports not_initialized for an unbound source" {
    var source = abi.Source{};
    var storage = [_]u8{0} ** 32;
    const good = abi.Buffer{ .data = &storage, .capacity = storage.len };
    try std.testing.expectEqual(
        abi.err.not_initialized,
        abi.ra8_audio_source_stream_start(&source, &good, noopCallback, null),
    );
}

test "stream_start reports not_supported when only the stream row is absent" {
    const no_stream = abi.SourceIface{ .get_info = Fake.getInfo, .capture = Fake.capture, .stop = Fake.stop };
    var fake = newFake();
    var source = abi.Source{ .iface = &no_stream, .ctx = @ptrCast(&fake) };
    var storage = [_]u8{0} ** 32;
    const good = abi.Buffer{ .data = &storage, .capacity = storage.len };
    try std.testing.expectEqual(
        abi.err.not_supported,
        abi.ra8_audio_source_stream_start(&source, &good, noopCallback, null),
    );
    try std.testing.expectEqual(@as(u32, 0), fake.info_calls);
}

test "stream_start reports not_initialized when the info row is absent too" {
    const only_stream = abi.SourceIface{ .stream_start = Fake.streamStart };
    var fake = newFake();
    var source = abi.Source{ .iface = &only_stream, .ctx = @ptrCast(&fake) };
    var storage = [_]u8{0} ** 32;
    const good = abi.Buffer{ .data = &storage, .capacity = storage.len };
    try std.testing.expectEqual(
        abi.err.not_initialized,
        abi.ra8_audio_source_stream_start(&source, &good, noopCallback, null),
    );
}

test "stream_start rejects a buffer smaller than one frame and forwards info errors" {
    var fake = newFake();
    var source = sourceFor(&fake);
    var storage = [_]u8{0} ** 32;
    const small = abi.Buffer{ .data = &storage, .capacity = 28 };
    try std.testing.expectEqual(
        abi.err.invalid_size,
        abi.ra8_audio_source_stream_start(&source, &small, noopCallback, null),
    );
    fake.info_status = 0x301;
    const good = abi.Buffer{ .data = &storage, .capacity = storage.len };
    try std.testing.expectEqual(
        @as(u16, 0x301),
        abi.ra8_audio_source_stream_start(&source, &good, noopCallback, null),
    );
    try std.testing.expectEqual(@as(u32, 0), fake.stream_calls);
}

test "stream_start dispatches the buffer and context it was given" {
    var fake = newFake();
    var source = sourceFor(&fake);
    var storage = [_]u8{0} ** 32;
    const good = abi.Buffer{ .data = &storage, .capacity = storage.len };
    var token: u32 = 5;
    try std.testing.expectEqual(
        abi.err.ok,
        abi.ra8_audio_source_stream_start(&source, &good, noopCallback, @ptrCast(&token)),
    );
    try std.testing.expectEqual(@as(u32, 1), fake.stream_calls);
    try std.testing.expectEqual(@intFromPtr(&good), @intFromPtr(fake.seen_buffer.?));
    try std.testing.expectEqual(@intFromPtr(&token), @intFromPtr(fake.seen_ctx.?));

    try std.testing.expectEqual(
        abi.err.ok,
        abi.ra8_audio_source_stream_start(&source, &good, noopCallback, @ptrCast(&token)),
    );
    try std.testing.expectEqual(@as(u32, 2), fake.stream_calls);
}

test "stream_start forwards a backend refusal" {
    var fake = newFake();
    fake.stream_status = abi.err.exists;
    var source = sourceFor(&fake);
    var storage = [_]u8{0} ** 32;
    const good = abi.Buffer{ .data = &storage, .capacity = storage.len };
    try std.testing.expectEqual(
        abi.err.exists,
        abi.ra8_audio_source_stream_start(&source, &good, noopCallback, null),
    );
}

// --- ra8_audio_source_stop ------------------------------------------------

test "stop rejects an absent handle and an unbound source" {
    try std.testing.expectEqual(abi.err.null_ptr, abi.ra8_audio_source_stop(null));
    var source = abi.Source{};
    try std.testing.expectEqual(abi.err.not_initialized, abi.ra8_audio_source_stop(&source));
    var iface_only = abi.Source{ .iface = &unbound_iface };
    try std.testing.expectEqual(abi.err.not_initialized, abi.ra8_audio_source_stop(&iface_only));
}

test "stop clears the handle only on success" {
    var fake = newFake();
    fake.stop_status = 0x204;
    var source = sourceFor(&fake);
    try std.testing.expectEqual(@as(u16, 0x204), abi.ra8_audio_source_stop(&source));
    try std.testing.expect(source.iface != null);
    try std.testing.expectEqual(@as(u32, 1), fake.stop_calls);

    fake.stop_status = 0;
    try std.testing.expectEqual(abi.err.ok, abi.ra8_audio_source_stop(&source));
    try std.testing.expect(source.iface == null);
    try std.testing.expect(source.ctx == null);
    try std.testing.expectEqual(abi.err.not_initialized, abi.ra8_audio_source_stop(&source));
}

// --- memory replay backend ------------------------------------------------

test "memory_init rejects each absent argument" {
    var source = abi.Source{};
    var state = memory.MemoryState{};
    var frame = fixture();
    try std.testing.expectEqual(abi.err.null_ptr, memory.ra8_audio_source_memory_init(null, &state, &frame));
    try std.testing.expectEqual(abi.err.null_ptr, memory.ra8_audio_source_memory_init(&source, null, &frame));
    try std.testing.expectEqual(abi.err.null_ptr, memory.ra8_audio_source_memory_init(&source, &state, null));
}

test "memory_init forwards frame-validation failures" {
    var source = abi.Source{};
    var state = memory.MemoryState{};
    var frame = fixture();
    frame.bytes = 31;
    try std.testing.expectEqual(abi.err.invalid_size, memory.ra8_audio_source_memory_init(&source, &state, &frame));
    frame = fixture();
    frame.data = null;
    try std.testing.expectEqual(abi.err.null_ptr, memory.ra8_audio_source_memory_init(&source, &state, &frame));
    try std.testing.expect(source.iface == null);
}

test "memory source replays the fixture into caller storage" {
    var source = abi.Source{};
    var state = memory.MemoryState{};
    var frame = fixture();
    try std.testing.expectEqual(abi.err.ok, memory.ra8_audio_source_memory_init(&source, &state, &frame));

    var info: abi.Info = .{};
    try std.testing.expectEqual(abi.err.ok, abi.ra8_audio_source_get_info(&source, &info));
    try std.testing.expectEqual(@as(u32, 32), info.frame_bytes);
    try std.testing.expectEqual(@as(u32, 8), info.samples_per_frame);
    try std.testing.expectEqual(@as(u8, 1), info.channels);
    try std.testing.expectEqual(@as(u8, 20), info.valid_bits);

    var storage = [_]i32{0} ** 8;
    const buffer = abi.Buffer{ .data = @ptrCast(&storage), .capacity = 32 };
    var captured: abi.Frame = .{};
    try std.testing.expectEqual(abi.err.ok, abi.ra8_audio_source_capture(&source, &buffer, &captured));
    try std.testing.expectEqualSlices(i32, &fixture_samples, &storage);
    try std.testing.expectEqual(@intFromPtr(&storage), @intFromPtr(captured.data.?));
    try std.testing.expectEqual(@as(u32, 42), captured.timestamp_ms);

    // Replay is repeatable and leaves the fixture untouched.
    @memset(&storage, 0);
    try std.testing.expectEqual(abi.err.ok, abi.ra8_audio_source_capture(&source, &buffer, &captured));
    try std.testing.expectEqualSlices(i32, &fixture_samples, &storage);
}

test "memory source has no streaming row" {
    var source = abi.Source{};
    var state = memory.MemoryState{};
    var frame = fixture();
    try std.testing.expectEqual(abi.err.ok, memory.ra8_audio_source_memory_init(&source, &state, &frame));
    var storage = [_]i32{0} ** 8;
    const buffer = abi.Buffer{ .data = @ptrCast(&storage), .capacity = 32 };
    try std.testing.expectEqual(
        abi.err.not_supported,
        abi.ra8_audio_source_stream_start(&source, &buffer, noopCallback, null),
    );
}

test "memory source rejects a buffer it cannot fill and stops cleanly" {
    var source = abi.Source{};
    var state = memory.MemoryState{};
    var frame = fixture();
    try std.testing.expectEqual(abi.err.ok, memory.ra8_audio_source_memory_init(&source, &state, &frame));
    var storage = [_]i32{0} ** 8;
    const small = abi.Buffer{ .data = @ptrCast(&storage), .capacity = 28 };
    var captured: abi.Frame = .{};
    try std.testing.expectEqual(abi.err.invalid_size, abi.ra8_audio_source_capture(&source, &small, &captured));
    try std.testing.expectEqual(abi.err.ok, abi.ra8_audio_source_stop(&source));
    try std.testing.expect(source.iface == null);
}
