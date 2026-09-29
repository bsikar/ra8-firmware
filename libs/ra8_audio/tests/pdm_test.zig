//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for the PDM backend. This file exports its own `ra8_pdm_*` and
//! clock fakes, so the backend's externs bind to them exactly as they bind to
//! the real HAL in a target build, and it captures the FIFO callback the
//! backend installs so the interrupt assembly path runs host-side.

const std = @import("std");
const pdm = @import("pdm");
const abi = pdm.abi;

const err_invalid_arg: u16 = 0x103;
const err_hw_init_failed: u16 = 0x201;
const err_hw_timeout: u16 = 0x203;
const err_nack: u16 = 0x407;

/// Everything the fake HAL records or injects for one test.
const Hal = struct {
    init_status: u16 = 0,
    configure_status: u16 = 0,
    start_status: u16 = 0,
    read_enable_status: u16 = 0,
    read_status: u16 = 0,
    stream_status: u16 = 0,
    stop_status: u16 = 0,
    deinit_status: u16 = 0,

    init_calls: u32 = 0,
    configure_calls: u32 = 0,
    start_calls: u32 = 0,
    read_enable_calls: u32 = 0,
    read_calls: u32 = 0,
    stream_calls: u32 = 0,
    stop_calls: u32 = 0,
    deinit_calls: u32 = 0,

    per_read: u32 = 64,
    next_sample: i32 = 100,
    seen_channel: u8 = 0xFF,
    seen_priority: u8 = 0xFF,
    seen_cfg_sinc_order: u8 = 0,
    installed: ?pdm.PdmDataCallback = null,
    installed_ctx: ?*anyopaque = null,
    now_ms: u32 = 0,
    delays: u32 = 0,
    last_delay_ms: u32 = 0,
};

var hal: Hal = .{};

fn resetHal() void {
    hal = .{};
}

export fn ra8_pdm_init() u16 {
    hal.init_calls += 1;
    return hal.init_status;
}

export fn ra8_pdm_deinit() u16 {
    hal.deinit_calls += 1;
    return hal.deinit_status;
}

export fn ra8_pdm_configure(ch: u8, cfg: *const pdm.PdmChannelCfg) u16 {
    hal.configure_calls += 1;
    hal.seen_channel = ch;
    hal.seen_cfg_sinc_order = cfg.sinc_order;
    return hal.configure_status;
}

export fn ra8_pdm_start(ch: u8) u16 {
    _ = ch;
    hal.start_calls += 1;
    return hal.start_status;
}

export fn ra8_pdm_read_enable(ch: u8) u16 {
    _ = ch;
    hal.read_enable_calls += 1;
    return hal.read_enable_status;
}

export fn ra8_pdm_read(ch: u8, out: [*]i32, max: u32, out_count: *u32) u16 {
    _ = ch;
    hal.read_calls += 1;
    if (hal.read_status != 0) {
        out_count.* = 0;
        return hal.read_status;
    }
    const give = @min(hal.per_read, max);
    var index: u32 = 0;
    while (index < give) : (index += 1) {
        out[index] = hal.next_sample;
        hal.next_sample += 1;
    }
    out_count.* = give;
    return 0;
}

export fn ra8_pdm_stream_enable(
    ch: u8,
    callback: ?pdm.PdmDataCallback,
    ctx: ?*anyopaque,
    priority: u8,
) u16 {
    hal.stream_calls += 1;
    hal.seen_channel = ch;
    hal.seen_priority = priority;
    if (hal.stream_status != 0) return hal.stream_status;
    hal.installed = callback;
    hal.installed_ctx = ctx;
    return 0;
}

export fn ra8_pdm_stop(ch: u8) u16 {
    _ = ch;
    hal.stop_calls += 1;
    return hal.stop_status;
}

export fn ra8_time_ms() u32 {
    return hal.now_ms;
}

export fn ra8_delay_ms(ms: u32) void {
    hal.delays += 1;
    hal.last_delay_ms = ms;
}

fn goodCfg() pdm.PdmCfg {
    var cfg = pdm.PdmCfg{
        .sample_rate_hz = 16000,
        .samples_per_frame = 8,
        .settle_ms = 25,
        .discard_samples = 0,
        .poll_attempts = 4,
        .channel = 2,
        .valid_bits = 24,
        .irq_priority = 7,
    };
    cfg.pdm.sinc_order = 3;
    return cfg;
}

/// Frames the test callback observed.
var seen_frames: u32 = 0;
var seen_last: abi.Frame = .{};
var seen_ctx_token: ?*anyopaque = null;

fn frameSink(ctx: ?*anyopaque, frame: ?*const abi.Frame) callconv(.c) void {
    seen_frames += 1;
    seen_ctx_token = ctx;
    if (frame) |view| seen_last = view.*;
}

fn resetSink() void {
    seen_frames = 0;
    seen_last = .{};
    seen_ctx_token = null;
}

fn openSource(source: *abi.Source, state: *pdm.PdmState) !void {
    const cfg = goodCfg();
    try std.testing.expectEqual(@as(u16, 0), pdm.ra8_audio_source_pdm_init(source, state, &cfg));
}

test "pdm_init rejects each absent argument before touching hardware" {
    resetHal();
    var source = abi.Source{};
    var state = pdm.PdmState{};
    const cfg = goodCfg();
    try std.testing.expectEqual(abi.err.null_ptr, pdm.ra8_audio_source_pdm_init(null, &state, &cfg));
    try std.testing.expectEqual(abi.err.null_ptr, pdm.ra8_audio_source_pdm_init(&source, null, &cfg));
    try std.testing.expectEqual(abi.err.null_ptr, pdm.ra8_audio_source_pdm_init(&source, &state, null));
    try std.testing.expectEqual(@as(u32, 0), hal.init_calls);
}

test "pdm_init clears both caller objects before validating" {
    resetHal();
    var source = abi.Source{ .iface = undefined, .ctx = null };
    source.iface = null;
    var state = pdm.PdmState{ .initialized = true, .stream_filled = 9 };
    var cfg = goodCfg();
    cfg.channel = 3;
    try std.testing.expectEqual(err_invalid_arg, pdm.ra8_audio_source_pdm_init(&source, &state, &cfg));
    try std.testing.expect(!state.initialized);
    try std.testing.expectEqual(@as(u32, 0), state.stream_filled);
    try std.testing.expect(source.iface == null);
    try std.testing.expectEqual(@as(u32, 0), hal.init_calls);
}

test "pdm_init rejects every invalid configuration field" {
    resetHal();
    var source = abi.Source{};
    var state = pdm.PdmState{};

    var cfg = goodCfg();
    cfg.sample_rate_hz = 0;
    try std.testing.expectEqual(err_invalid_arg, pdm.ra8_audio_source_pdm_init(&source, &state, &cfg));
    cfg = goodCfg();
    cfg.samples_per_frame = 0;
    try std.testing.expectEqual(err_invalid_arg, pdm.ra8_audio_source_pdm_init(&source, &state, &cfg));
    cfg = goodCfg();
    cfg.poll_attempts = 0;
    try std.testing.expectEqual(err_invalid_arg, pdm.ra8_audio_source_pdm_init(&source, &state, &cfg));
    cfg = goodCfg();
    cfg.valid_bits = 0;
    try std.testing.expectEqual(err_invalid_arg, pdm.ra8_audio_source_pdm_init(&source, &state, &cfg));
    cfg = goodCfg();
    cfg.valid_bits = 33;
    try std.testing.expectEqual(err_invalid_arg, pdm.ra8_audio_source_pdm_init(&source, &state, &cfg));
    cfg = goodCfg();
    cfg.samples_per_frame = 0x4000_0000;
    try std.testing.expectEqual(abi.err.invalid_size, pdm.ra8_audio_source_pdm_init(&source, &state, &cfg));
    try std.testing.expectEqual(@as(u32, 0), hal.init_calls);
}

test "pdm_init brings the channel up in order and settles it" {
    resetHal();
    hal.now_ms = 1234;
    var source = abi.Source{};
    var state = pdm.PdmState{};
    try openSource(&source, &state);

    try std.testing.expectEqual(@as(u32, 1), hal.init_calls);
    try std.testing.expectEqual(@as(u32, 1), hal.configure_calls);
    try std.testing.expectEqual(@as(u32, 1), hal.start_calls);
    try std.testing.expectEqual(@as(u32, 1), hal.read_enable_calls);
    try std.testing.expectEqual(@as(u32, 1), hal.delays);
    try std.testing.expectEqual(@as(u32, 25), hal.last_delay_ms);
    try std.testing.expectEqual(@as(u8, 2), hal.seen_channel);
    try std.testing.expectEqual(@as(u8, 3), hal.seen_cfg_sinc_order);
    try std.testing.expectEqual(@as(u32, 0), hal.deinit_calls);

    // The advertised contract is mono PCM-S32LE at four bytes per sample.
    var info: abi.Info = .{};
    try std.testing.expectEqual(abi.err.ok, abi.ra8_audio_source_get_info(&source, &info));
    try std.testing.expectEqual(@as(u32, 32), info.frame_bytes);
    try std.testing.expectEqual(@as(u32, 8), info.samples_per_frame);
    try std.testing.expectEqual(@as(u32, 16000), info.sample_rate_hz);
    try std.testing.expectEqual(@as(u8, 1), info.channels);
    try std.testing.expectEqual(@as(u8, 24), info.valid_bits);
    try std.testing.expectEqual(@as(u8, 1), info.format);
}

test "a failing HAL init leaves the source unbound and nothing torn down" {
    resetHal();
    hal.init_status = err_hw_init_failed;
    var source = abi.Source{};
    var state = pdm.PdmState{};
    const cfg = goodCfg();
    try std.testing.expectEqual(err_hw_init_failed, pdm.ra8_audio_source_pdm_init(&source, &state, &cfg));
    try std.testing.expect(source.iface == null);
    try std.testing.expectEqual(@as(u32, 0), hal.configure_calls);
    try std.testing.expectEqual(@as(u32, 0), hal.deinit_calls);
}

test "a failing configure deinitializes without stopping" {
    resetHal();
    hal.configure_status = err_invalid_arg;
    var source = abi.Source{};
    var state = pdm.PdmState{};
    const cfg = goodCfg();
    try std.testing.expectEqual(err_invalid_arg, pdm.ra8_audio_source_pdm_init(&source, &state, &cfg));
    try std.testing.expectEqual(@as(u32, 0), hal.start_calls);
    try std.testing.expectEqual(@as(u32, 1), hal.deinit_calls);
    try std.testing.expectEqual(@as(u32, 0), hal.stop_calls);
    try std.testing.expect(!state.initialized);
}

test "a failing start deinitializes without stopping" {
    resetHal();
    hal.start_status = err_hw_init_failed;
    var source = abi.Source{};
    var state = pdm.PdmState{};
    const cfg = goodCfg();
    try std.testing.expectEqual(err_hw_init_failed, pdm.ra8_audio_source_pdm_init(&source, &state, &cfg));
    try std.testing.expectEqual(@as(u32, 1), hal.deinit_calls);
    try std.testing.expectEqual(@as(u32, 0), hal.stop_calls);
    try std.testing.expectEqual(@as(u32, 0), hal.read_enable_calls);
}

test "a failing read-enable stops and deinitializes" {
    resetHal();
    hal.read_enable_status = err_invalid_arg;
    var source = abi.Source{};
    var state = pdm.PdmState{};
    const cfg = goodCfg();
    try std.testing.expectEqual(err_invalid_arg, pdm.ra8_audio_source_pdm_init(&source, &state, &cfg));
    try std.testing.expectEqual(@as(u32, 1), hal.stop_calls);
    try std.testing.expectEqual(@as(u32, 1), hal.deinit_calls);
    try std.testing.expect(source.iface == null);
}

test "startup samples are discarded one at a time" {
    resetHal();
    hal.per_read = 1;
    var source = abi.Source{};
    var state = pdm.PdmState{};
    var cfg = goodCfg();
    cfg.discard_samples = 5;
    try std.testing.expectEqual(@as(u16, 0), pdm.ra8_audio_source_pdm_init(&source, &state, &cfg));
    try std.testing.expectEqual(@as(u32, 5), hal.read_calls);
}

test "a discard read that never fills times out and tears the channel down" {
    resetHal();
    hal.per_read = 0;
    var source = abi.Source{};
    var state = pdm.PdmState{};
    var cfg = goodCfg();
    cfg.discard_samples = 2;
    try std.testing.expectEqual(err_hw_timeout, pdm.ra8_audio_source_pdm_init(&source, &state, &cfg));
    try std.testing.expectEqual(@as(u32, 4), hal.read_calls);
    try std.testing.expectEqual(@as(u32, 1), hal.stop_calls);
    try std.testing.expectEqual(@as(u32, 1), hal.deinit_calls);
}

test "a transport error during discard is forwarded verbatim" {
    resetHal();
    hal.read_status = err_nack;
    var source = abi.Source{};
    var state = pdm.PdmState{};
    var cfg = goodCfg();
    cfg.discard_samples = 3;
    try std.testing.expectEqual(err_nack, pdm.ra8_audio_source_pdm_init(&source, &state, &cfg));
    try std.testing.expectEqual(@as(u32, 1), hal.read_calls);
    try std.testing.expectEqual(@as(u32, 1), hal.stop_calls);
}

test "an uninitialized state answers not_initialized on every row" {
    resetHal();
    var state = pdm.PdmState{};
    var source = abi.Source{};
    try openSource(&source, &state);
    state.initialized = false;

    var info: abi.Info = .{};
    try std.testing.expectEqual(abi.err.not_initialized, abi.ra8_audio_source_get_info(&source, &info));
    var storage = [_]i32{0} ** 8;
    const buffer = abi.Buffer{ .data = @ptrCast(&storage), .capacity = 32 };
    var frame: abi.Frame = .{};
    try std.testing.expectEqual(abi.err.not_initialized, abi.ra8_audio_source_capture(&source, &buffer, &frame));
    try std.testing.expectEqual(
        abi.err.not_initialized,
        abi.ra8_audio_source_stream_start(&source, &buffer, frameSink, null),
    );
    try std.testing.expectEqual(abi.err.not_initialized, abi.ra8_audio_source_stop(&source));
}

test "polled capture fills the caller buffer and timestamps the frame" {
    resetHal();
    hal.now_ms = 777;
    var source = abi.Source{};
    var state = pdm.PdmState{};
    try openSource(&source, &state);

    var storage = [_]i32{0} ** 8;
    const buffer = abi.Buffer{ .data = @ptrCast(&storage), .capacity = 32 };
    var frame: abi.Frame = .{};
    try std.testing.expectEqual(abi.err.ok, abi.ra8_audio_source_capture(&source, &buffer, &frame));
    try std.testing.expectEqual(@intFromPtr(&storage), @intFromPtr(frame.data.?));
    try std.testing.expectEqual(@as(u32, 777), frame.timestamp_ms);
    try std.testing.expectEqual(@as(u32, 32), frame.bytes);
    try std.testing.expectEqual(@as(i32, 100), storage[0]);
    try std.testing.expectEqual(@as(i32, 107), storage[7]);
}

test "capture collects a frame across several partial FIFO reads" {
    resetHal();
    hal.per_read = 3;
    var source = abi.Source{};
    var state = pdm.PdmState{};
    try openSource(&source, &state);

    var storage = [_]i32{0} ** 8;
    const buffer = abi.Buffer{ .data = @ptrCast(&storage), .capacity = 32 };
    var frame: abi.Frame = .{};
    try std.testing.expectEqual(abi.err.ok, abi.ra8_audio_source_capture(&source, &buffer, &frame));
    try std.testing.expectEqual(@as(u32, 3), hal.read_calls);
    try std.testing.expectEqual(@as(i32, 107), storage[7]);
}

test "capture times out when the attempt budget is spent" {
    resetHal();
    hal.per_read = 1;
    var source = abi.Source{};
    var state = pdm.PdmState{};
    try openSource(&source, &state);

    var storage = [_]i32{0} ** 8;
    const buffer = abi.Buffer{ .data = @ptrCast(&storage), .capacity = 32 };
    var frame: abi.Frame = .{};
    // Four attempts, one sample each, eight samples wanted.
    try std.testing.expectEqual(err_hw_timeout, abi.ra8_audio_source_capture(&source, &buffer, &frame));
    try std.testing.expectEqual(@as(u32, 4), hal.read_calls);
}

test "capture forwards a FIFO read error" {
    resetHal();
    hal.read_status = err_nack;
    var source = abi.Source{};
    var state = pdm.PdmState{};
    try openSource(&source, &state);

    var storage = [_]i32{0} ** 8;
    const buffer = abi.Buffer{ .data = @ptrCast(&storage), .capacity = 32 };
    var frame: abi.Frame = .{};
    try std.testing.expectEqual(err_nack, abi.ra8_audio_source_capture(&source, &buffer, &frame));
}

test "capture checks the buffer size before its alignment" {
    resetHal();
    var source = abi.Source{};
    var state = pdm.PdmState{};
    try openSource(&source, &state);

    var storage align(4) = [_]u8{0} ** 64;
    var frame: abi.Frame = .{};
    const small = abi.Buffer{ .data = @ptrCast(&storage), .capacity = 28 };
    try std.testing.expectEqual(abi.err.invalid_size, abi.ra8_audio_source_capture(&source, &small, &frame));

    const misaligned = abi.Buffer{ .data = @ptrCast(storage[1..].ptr), .capacity = 32 };
    try std.testing.expectEqual(abi.err.invalid_arg, abi.ra8_audio_source_capture(&source, &misaligned, &frame));
    try std.testing.expectEqual(@as(u32, 0), hal.read_calls);
}

test "stream_start refuses a misaligned assembly buffer and a second stream" {
    resetHal();
    var source = abi.Source{};
    var state = pdm.PdmState{};
    try openSource(&source, &state);

    var storage align(4) = [_]u8{0} ** 64;
    const misaligned = abi.Buffer{ .data = @ptrCast(storage[2..].ptr), .capacity = 32 };
    try std.testing.expectEqual(
        abi.err.invalid_arg,
        abi.ra8_audio_source_stream_start(&source, &misaligned, frameSink, null),
    );
    try std.testing.expect(!state.streaming);

    const good = abi.Buffer{ .data = @ptrCast(&storage), .capacity = 32 };
    try std.testing.expectEqual(abi.err.ok, abi.ra8_audio_source_stream_start(&source, &good, frameSink, null));
    try std.testing.expect(state.streaming);
    try std.testing.expectEqual(abi.err.exists, abi.ra8_audio_source_stream_start(&source, &good, frameSink, null));
    try std.testing.expectEqual(@as(u32, 1), hal.stream_calls);
}

test "a failing stream enable clears every retained stream pointer" {
    resetHal();
    hal.stream_status = err_invalid_arg;
    var source = abi.Source{};
    var state = pdm.PdmState{};
    try openSource(&source, &state);

    var storage align(4) = [_]i32{0} ** 8;
    const good = abi.Buffer{ .data = @ptrCast(&storage), .capacity = 32 };
    try std.testing.expectEqual(
        err_invalid_arg,
        abi.ra8_audio_source_stream_start(&source, &good, frameSink, null),
    );
    try std.testing.expect(!state.streaming);
    try std.testing.expect(state.stream_data == null);
    try std.testing.expect(state.stream_callback == null);
    try std.testing.expect(state.stream_ctx == null);
}

test "the installed FIFO handler assembles and publishes whole frames" {
    resetHal();
    resetSink();
    hal.now_ms = 4242;
    var source = abi.Source{};
    var state = pdm.PdmState{};
    try openSource(&source, &state);

    var assembly = [_]i32{0} ** 8;
    const good = abi.Buffer{ .data = @ptrCast(&assembly), .capacity = 32 };
    var token: u32 = 3;
    try std.testing.expectEqual(
        abi.err.ok,
        abi.ra8_audio_source_stream_start(&source, &good, frameSink, @ptrCast(&token)),
    );
    try std.testing.expectEqual(@as(u8, 2), hal.seen_channel);
    try std.testing.expectEqual(@as(u8, 7), hal.seen_priority);

    const handler = hal.installed.?;
    const ctx = hal.installed_ctx.?;
    const first = [_]i32{ 1, 2, 3 };
    handler(ctx, &first, first.len);
    try std.testing.expectEqual(@as(u32, 0), seen_frames);
    try std.testing.expectEqual(@as(u32, 3), state.stream_filled);
    try std.testing.expectEqual(@as(u32, 4242), state.stream_timestamp_ms);

    // A later span carries the rest of this frame plus the whole next one.
    hal.now_ms = 5000;
    const rest = [_]i32{ 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
    handler(ctx, &rest, rest.len);
    try std.testing.expectEqual(@as(u32, 2), seen_frames);
    try std.testing.expectEqual(@as(u32, 0), state.stream_filled);
    try std.testing.expectEqual(@as(u32, 5000), seen_last.timestamp_ms);
    try std.testing.expectEqual(@as(u32, 32), seen_last.bytes);
    try std.testing.expectEqual(@as(u32, 8), seen_last.sample_count);
    try std.testing.expectEqual(@as(u8, 1), seen_last.channels);
    try std.testing.expectEqual(@intFromPtr(&assembly), @intFromPtr(seen_last.data.?));
    try std.testing.expectEqual(@intFromPtr(&token), @intFromPtr(seen_ctx_token.?));
    // The second frame is the tail of the span, samples 9..16.
    try std.testing.expectEqual(@as(i32, 9), assembly[0]);
    try std.testing.expectEqual(@as(i32, 16), assembly[7]);
    // The first frame's timestamp was taken when its first sample landed.
    try std.testing.expectEqual(@as(u16, 0), abi.ra8_audio_frame_validate(&seen_last));
}

test "the handler ignores spans once the stream is no longer active" {
    resetHal();
    resetSink();
    var source = abi.Source{};
    var state = pdm.PdmState{};
    try openSource(&source, &state);

    var assembly = [_]i32{0} ** 8;
    const good = abi.Buffer{ .data = @ptrCast(&assembly), .capacity = 32 };
    try std.testing.expectEqual(abi.err.ok, abi.ra8_audio_source_stream_start(&source, &good, frameSink, null));
    const handler = hal.installed.?;
    const ctx = hal.installed_ctx.?;

    state.streaming = false;
    const span = [_]i32{ 1, 2, 3, 4, 5, 6, 7, 8 };
    handler(ctx, &span, span.len);
    try std.testing.expectEqual(@as(u32, 0), seen_frames);
    try std.testing.expectEqual(@as(u32, 0), state.stream_filled);

    // An absent context is ignored rather than dereferenced.
    handler(null, &span, span.len);
    try std.testing.expectEqual(@as(u32, 0), seen_frames);
}

test "stop stops before deinitializing and only then marks the source down" {
    resetHal();
    var source = abi.Source{};
    var state = pdm.PdmState{};
    try openSource(&source, &state);

    hal.stop_status = err_invalid_arg;
    try std.testing.expectEqual(err_invalid_arg, abi.ra8_audio_source_stop(&source));
    try std.testing.expectEqual(@as(u32, 0), hal.deinit_calls);
    try std.testing.expect(state.initialized);
    try std.testing.expect(source.iface != null);

    hal.stop_status = 0;
    hal.deinit_status = err_invalid_arg;
    try std.testing.expectEqual(err_invalid_arg, abi.ra8_audio_source_stop(&source));
    try std.testing.expect(state.initialized);

    hal.deinit_status = 0;
    try std.testing.expectEqual(abi.err.ok, abi.ra8_audio_source_stop(&source));
    try std.testing.expect(!state.initialized);
    try std.testing.expect(source.iface == null);
}
