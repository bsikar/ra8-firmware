//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! RA8 PDM-IF audio-source backend. Adapts bounded FIFO polling and the
//! interrupt stream to the generic facade while every buffer and all mutable
//! state stay with the caller.
//!
//! The PDM HAL and the millisecond clock stay `extern`, so the host fixture
//! in `tests/hal/src/test_ra8_pdm.c` substitutes them at link time exactly as
//! it did for the C implementation.

const std = @import("std");
pub const abi = @import("ra8_audio_abi.zig");
pub const core = abi.core;

/// `ra8_pdm_channel_cfg_t`: filter/mode registers for one channel.
pub const PdmChannelCfg = extern struct {
    sinc_order: u8 = 0,
    clock_div: u8 = 0,
    sinc_dec: u8 = 0,
    sinc_range: u8 = 0,
    data_shift: u8 = 0,
    edge: u8 = 0,
    hpf_shift: u8 = 0,
    cf_shift: u8 = 0,
    lpf_shift: u8 = 0,
    rx_threshold: u8 = 0,
    hpf_s0: u16 = 0,
    hpf_k1: u16 = 0,
    hpf_h: [2]u16 = @splat(0),
    comp_h: [11]u16 = @splat(0),
    lpf_h0: u16 = 0,
    lpf_h1: [20]u16 = @splat(0),
};

/// `ra8_audio_source_pdm_cfg_t`.
pub const PdmCfg = extern struct {
    pdm: PdmChannelCfg = .{},
    sample_rate_hz: u32 = 0,
    samples_per_frame: u32 = 0,
    settle_ms: u32 = 0,
    discard_samples: u32 = 0,
    poll_attempts: u32 = 0,
    channel: u8 = 0,
    valid_bits: u8 = 0,
    irq_priority: u8 = 0,
};

/// `ra8_audio_source_pdm_state_t`: caller-owned mutable backend state.
pub const PdmState = extern struct {
    info: abi.Info = .{},
    poll_attempts: u32 = 0,
    stream_data: ?*anyopaque = null,
    stream_callback: ?abi.FrameCallback = null,
    stream_ctx: ?*anyopaque = null,
    stream_filled: u32 = 0,
    stream_timestamp_ms: u32 = 0,
    channel: u8 = 0,
    irq_priority: u8 = 0,
    initialized: bool = false,
    streaming: bool = false,
};

const ptr_bytes = @sizeOf(usize);

comptime {
    // The coefficient block is 2-byte aligned, so the u32 policy fields that
    // follow it sit at 84 on every target this repo builds for.
    std.debug.assert(@sizeOf(PdmChannelCfg) == 82);
    std.debug.assert(@alignOf(PdmChannelCfg) == 2);
    std.debug.assert(@offsetOf(PdmChannelCfg, "hpf_s0") == 10);
    std.debug.assert(@offsetOf(PdmChannelCfg, "hpf_h") == 14);
    std.debug.assert(@offsetOf(PdmChannelCfg, "comp_h") == 18);
    std.debug.assert(@offsetOf(PdmChannelCfg, "lpf_h0") == 40);
    std.debug.assert(@offsetOf(PdmChannelCfg, "lpf_h1") == 42);

    std.debug.assert(@sizeOf(PdmCfg) == 108);
    std.debug.assert(@alignOf(PdmCfg) == 4);
    std.debug.assert(@offsetOf(PdmCfg, "sample_rate_hz") == 84);
    std.debug.assert(@offsetOf(PdmCfg, "samples_per_frame") == 88);
    std.debug.assert(@offsetOf(PdmCfg, "settle_ms") == 92);
    std.debug.assert(@offsetOf(PdmCfg, "discard_samples") == 96);
    std.debug.assert(@offsetOf(PdmCfg, "poll_attempts") == 100);
    std.debug.assert(@offsetOf(PdmCfg, "channel") == 104);
    std.debug.assert(@offsetOf(PdmCfg, "valid_bits") == 105);
    std.debug.assert(@offsetOf(PdmCfg, "irq_priority") == 106);

    // The four trailing bytes ride inside one pointer-sized slot, so the state
    // is 8 pointers wide on the host and 11 words on 32-bit Arm.
    std.debug.assert(@offsetOf(PdmState, "info") == 0);
    std.debug.assert(@offsetOf(PdmState, "poll_attempts") == 16);
    std.debug.assert(@offsetOf(PdmState, "stream_data") == std.mem.alignForward(usize, 20, ptr_bytes));
    std.debug.assert(@offsetOf(PdmState, "stream_callback") == @offsetOf(PdmState, "stream_data") + ptr_bytes);
    std.debug.assert(@offsetOf(PdmState, "stream_ctx") == @offsetOf(PdmState, "stream_data") + ptr_bytes * 2);
    std.debug.assert(@offsetOf(PdmState, "stream_filled") == @offsetOf(PdmState, "stream_data") + ptr_bytes * 3);
    std.debug.assert(@offsetOf(PdmState, "stream_timestamp_ms") == @offsetOf(PdmState, "stream_filled") + 4);
    std.debug.assert(@offsetOf(PdmState, "channel") == @offsetOf(PdmState, "stream_filled") + 8);
    std.debug.assert(@offsetOf(PdmState, "irq_priority") == @offsetOf(PdmState, "channel") + 1);
    std.debug.assert(@offsetOf(PdmState, "initialized") == @offsetOf(PdmState, "channel") + 2);
    std.debug.assert(@offsetOf(PdmState, "streaming") == @offsetOf(PdmState, "channel") + 3);
}

/// `ra8_pdm_data_callback_t`.
pub const PdmDataCallback = *const fn (?*anyopaque, ?[*]const i32, u32) callconv(.c) void;

extern fn ra8_pdm_init() u16;
extern fn ra8_pdm_deinit() u16;
extern fn ra8_pdm_configure(ch: u8, cfg: *const PdmChannelCfg) u16;
extern fn ra8_pdm_start(ch: u8) u16;
extern fn ra8_pdm_read_enable(ch: u8) u16;
extern fn ra8_pdm_read(ch: u8, out: [*]i32, max: u32, out_count: *u32) u16;
extern fn ra8_pdm_stream_enable(
    ch: u8,
    callback: ?PdmDataCallback,
    ctx: ?*anyopaque,
    priority: u8,
) u16;
extern fn ra8_pdm_stop(ch: u8) u16;
extern fn ra8_time_ms() u32;
extern fn ra8_delay_ms(ms: u32) void;

/// One PDM channel viewed as a bounded sample reader for `core.fillSamples`.
const FifoReader = struct {
    channel: u8,

    pub fn read(self: *FifoReader, out: []i32) core.ReadOutcome {
        var got: u32 = 0;
        const status = ra8_pdm_read(self.channel, out.ptr, @intCast(out.len), &got);
        return .{ .status = status, .got = got };
    }
};

fn fillStatusErr(status: core.FillStatus) u16 {
    return switch (status) {
        .complete => abi.err.ok,
        .timeout => abi.err.hw_timeout,
        .failed => |code| code,
    };
}

fn constStateOf(ctx: *anyopaque) *const PdmState {
    return @ptrCast(@alignCast(ctx));
}

fn stateOf(ctx: *anyopaque) *PdmState {
    return @ptrCast(@alignCast(ctx));
}

fn pdmGetInfo(ctx: ?*anyopaque, out_info: ?*abi.Info) callconv(.c) u16 {
    const context = ctx orelse return abi.err.not_initialized;
    const state = constStateOf(context);
    if (!state.initialized) return abi.err.not_initialized;
    const out = out_info orelse return abi.err.null_ptr;
    out.* = state.info;
    return abi.err.ok;
}

fn pdmCapture(
    ctx: ?*anyopaque,
    buffer: ?*const abi.Buffer,
    out_frame: ?*abi.Frame,
) callconv(.c) u16 {
    const context = ctx orelse return abi.err.not_initialized;
    const state = constStateOf(context);
    if (!state.initialized) return abi.err.not_initialized;
    const span = buffer orelse return abi.err.null_ptr;
    if (!core.capacityFits(span.capacity, state.info.frame_bytes)) return abi.err.invalid_size;
    const data = span.data orelse return abi.err.null_ptr;
    if (!core.pdmBufferAligned(@intFromPtr(data))) return abi.err.invalid_arg;
    const out = out_frame orelse return abi.err.null_ptr;

    const timestamp_ms = ra8_time_ms();
    var reader = FifoReader{ .channel = state.channel };
    const samples: [*]i32 = @ptrCast(@alignCast(data));
    const status = fillStatusErr(core.fillSamples(
        &reader,
        samples[0..state.info.samples_per_frame],
        state.poll_attempts,
    ));
    if (status != abi.err.ok) return status;

    out.* = .{
        .data = data,
        .bytes = state.info.frame_bytes,
        .sample_count = state.info.samples_per_frame,
        .sample_rate_hz = state.info.sample_rate_hz,
        .timestamp_ms = timestamp_ms,
        .channels = state.info.channels,
        .valid_bits = state.info.valid_bits,
        .format = state.info.format,
    };
    return abi.err.ok;
}

/// FIFO span handler installed with the HAL. Runs in interrupt context and
/// performs only bounded copies into the caller's assembly buffer.
fn pdmStreamData(ctx: ?*anyopaque, samples: ?[*]const i32, count: u32) callconv(.c) void {
    const context = ctx orelse return;
    const state = stateOf(context);
    if (!state.streaming) return;
    const origin = samples orelse return;
    const assembly = state.stream_data orelse return;
    const publish = state.stream_callback orelse return;
    const destination: [*]i32 = @ptrCast(@alignCast(assembly));

    var consumed: u32 = 0;
    while (consumed < count) {
        if (state.stream_filled == 0) state.stream_timestamp_ms = ra8_time_ms();
        const remaining = state.info.samples_per_frame -| state.stream_filled;
        const copy = core.chunkLen(count - consumed, remaining);
        // Only reachable on a hand-built state whose frame is zero samples
        // long; the C would have spun here forever on an unsigned underflow.
        if (copy == 0) return;
        @memcpy(
            destination[state.stream_filled..][0..copy],
            origin[consumed..][0..copy],
        );
        state.stream_filled += copy;
        consumed += copy;
        if (state.stream_filled == state.info.samples_per_frame) {
            const frame = abi.Frame{
                .data = assembly,
                .bytes = state.info.frame_bytes,
                .sample_count = state.info.samples_per_frame,
                .sample_rate_hz = state.info.sample_rate_hz,
                .timestamp_ms = state.stream_timestamp_ms,
                .channels = state.info.channels,
                .valid_bits = state.info.valid_bits,
                .format = state.info.format,
            };
            state.stream_filled = 0;
            publish(state.stream_ctx, &frame);
        }
    }
}

fn pdmStreamStart(
    ctx: ?*anyopaque,
    buffer: ?*const abi.Buffer,
    callback: ?abi.FrameCallback,
    callback_ctx: ?*anyopaque,
) callconv(.c) u16 {
    const context = ctx orelse return abi.err.not_initialized;
    const state = stateOf(context);
    if (!state.initialized) return abi.err.not_initialized;
    if (state.streaming) return abi.err.exists;
    const span = buffer orelse return abi.err.null_ptr;
    const data = span.data orelse return abi.err.null_ptr;
    if (!core.pdmBufferAligned(@intFromPtr(data))) return abi.err.invalid_arg;

    state.stream_data = data;
    state.stream_callback = callback;
    state.stream_ctx = callback_ctx;
    state.stream_filled = 0;
    state.stream_timestamp_ms = 0;
    state.streaming = true;
    const status = ra8_pdm_stream_enable(state.channel, pdmStreamData, state, state.irq_priority);
    if (status != abi.err.ok) {
        state.streaming = false;
        state.stream_data = null;
        state.stream_callback = null;
        state.stream_ctx = null;
    }
    return status;
}

fn pdmStop(ctx: ?*anyopaque) callconv(.c) u16 {
    const context = ctx orelse return abi.err.not_initialized;
    const state = stateOf(context);
    if (!state.initialized) return abi.err.not_initialized;
    const stop_status = ra8_pdm_stop(state.channel);
    if (stop_status != abi.err.ok) return stop_status;
    const deinit_status = ra8_pdm_deinit();
    if (deinit_status == abi.err.ok) state.initialized = false;
    return deinit_status;
}

/// `s_pdm_source_iface`.
const pdm_iface: abi.SourceIface = .{
    .get_info = pdmGetInfo,
    .capture = pdmCapture,
    .stream_start = pdmStreamStart,
    .stop = pdmStop,
};

fn cfgErr(fault: core.PdmCfgFault) u16 {
    return switch (fault) {
        .ok => abi.err.ok,
        .bad_channel,
        .zero_sample_rate,
        .zero_samples_per_frame,
        .zero_poll_attempts,
        .zero_valid_bits,
        .valid_bits_too_wide,
        => abi.err.invalid_arg,
        .frame_bytes_overflow => abi.err.invalid_size,
    };
}

/// `internal_pdm_prepare_hardware`: initialize, configure, start, settle,
/// read-enable, then burn the requested startup samples. Hardware that was
/// brought up is torn down again on any failure.
fn prepareHardware(cfg: *const PdmCfg) u16 {
    var status = ra8_pdm_init();
    if (status != abi.err.ok) return status;
    status = ra8_pdm_configure(cfg.channel, &cfg.pdm);
    if (status == abi.err.ok) status = ra8_pdm_start(cfg.channel);
    if (status != abi.err.ok) {
        _ = ra8_pdm_deinit();
        return status;
    }
    ra8_delay_ms(cfg.settle_ms);
    status = ra8_pdm_read_enable(cfg.channel);
    var discard: i32 = 0;
    var index: u32 = 0;
    while (index < cfg.discard_samples) : (index += 1) {
        if (status != abi.err.ok) break;
        var reader = FifoReader{ .channel = cfg.channel };
        status = fillStatusErr(core.fillSamples(&reader, (&discard)[0..1], cfg.poll_attempts));
    }
    if (status != abi.err.ok) {
        _ = ra8_pdm_stop(cfg.channel);
        _ = ra8_pdm_deinit();
    }
    return status;
}

pub export fn ra8_audio_source_pdm_init(
    source: ?*abi.Source,
    state: ?*PdmState,
    cfg: ?*const PdmCfg,
) u16 {
    const handle = source orelse return abi.err.null_ptr;
    const backing = state orelse return abi.err.null_ptr;
    const config = cfg orelse return abi.err.null_ptr;
    handle.* = .{};
    backing.* = .{};

    const fault = core.validatePdmCfg(
        config.channel,
        config.sample_rate_hz,
        config.samples_per_frame,
        config.poll_attempts,
        config.valid_bits,
    );
    if (fault != .ok) return cfgErr(fault);
    const frame_bytes: u32 = @intCast(core.pdmFrameBytes(config.samples_per_frame));

    const status = prepareHardware(config);
    if (status != abi.err.ok) return status;

    backing.* = .{
        .info = .{
            .frame_bytes = frame_bytes,
            .samples_per_frame = config.samples_per_frame,
            .sample_rate_hz = config.sample_rate_hz,
            .channels = 1,
            .valid_bits = config.valid_bits,
            .format = core.format_pcm_s32le,
        },
        .poll_attempts = config.poll_attempts,
        .channel = config.channel,
        .irq_priority = config.irq_priority,
        .initialized = true,
    };
    handle.* = .{ .iface = &pdm_iface, .ctx = @ptrCast(backing) };
    return abi.err.ok;
}
