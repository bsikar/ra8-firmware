//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! CEU capture-source backend: the register, cache and delay work that acts on
//! the answers `src/internal/source_ceu.zig` computes. Binds the Ring-3 CEU HAL
//! as a `ra8_camera_source_t` and DMA-writes into caller-owned storage.
//!
//! Two host suites reach the poll loop and the capture entry directly, so both
//! are exported under the `priv_cam_ceu_` prefix and declared in
//! `src/ra8_camera_source_ceu_private.h`; nothing else here is visible outside
//! the archive.

const std = @import("std");
const abi = @import("abi_types.zig");
const ceu = @import("ceu_abi.zig");

pub const core = abi.core;
pub const err = abi.err;
pub const policy = @import("internal/source_ceu.zig");

/// Re-exported so one test binary sees the same types the archive compiled.
pub const Buffer = core.Buffer;
pub const Frame = core.Frame;
pub const Info = core.Info;
pub const Source = abi.Source;
pub const Status = ceu.Status;
pub const CeuConfig = ceu.Config;
pub const CeuBuffers = ceu.Buffers;

/// `ra8_camera_source_ceu_cfg_t`: the HAL descriptor plus the metadata and
/// poll bounds the backend enforces.
pub const Cfg = extern struct {
    ceu: ceu.Config = .{},
    output: core.Info = .{},
    poll_interval_ms: u32 = 0,
    poll_attempts: u32 = 0,
};

/// `ra8_camera_source_ceu_state_t`: caller-owned backend state. Private after
/// init, except `last_events`, which the diagnostic accessor publishes.
pub const State = extern struct {
    info: core.Info = .{},
    capture_format: u8 = 0,
    poll_interval_ms: u32 = 0,
    poll_attempts: u32 = 0,
    last_events: u32 = 0,
    initialized: bool = false,
};

comptime {
    std.debug.assert(@offsetOf(Cfg, "ceu") == 0);
    std.debug.assert(@offsetOf(Cfg, "output") == 60);
    std.debug.assert(@offsetOf(Cfg, "poll_interval_ms") == 76);
    std.debug.assert(@offsetOf(Cfg, "poll_attempts") == 80);
    std.debug.assert(@sizeOf(Cfg) == 84);

    std.debug.assert(@offsetOf(State, "info") == 0);
    std.debug.assert(@offsetOf(State, "capture_format") == 16);
    std.debug.assert(@offsetOf(State, "poll_interval_ms") == 20);
    std.debug.assert(@offsetOf(State, "poll_attempts") == 24);
    std.debug.assert(@offsetOf(State, "last_events") == 28);
    std.debug.assert(@offsetOf(State, "initialized") == 32);
    std.debug.assert(@sizeOf(State) == 36);
}

extern fn ra8_cache_dcache_clean_invalidate_by_addr(addr: ?*const anyopaque, size: u32) callconv(.c) u16;
extern fn ra8_cache_dcache_invalidate_by_addr(addr: ?*const anyopaque, size: u32) callconv(.c) u16;
extern fn ra8_delay_ms(ms: u32) callconv(.c) void;

/// `ra8_err_t` values only the CEU path produces.
const hw = struct {
    pub const timeout: u16 = 0x203;
    pub const error_: u16 = 0x204;
};

/// Abandon an in-flight capture and clear every latch, reporting the reset's
/// own failure in place of the fault it was clearing.
fn resetAndClear() u16 {
    const reset_err = ceu.ra8_ceu_reset();
    _ = ceu.ra8_ceu_clear_status(policy.events.mask_all);
    return reset_err;
}

/// Poll bounded CEU status until completion, a fatal fault, or expiry.
fn waitForFrame(state: *State, out_bytes: *u32) u16 {
    var attempt: u32 = 0;
    while (attempt < state.poll_attempts) : (attempt += 1) {
        var status: ceu.Status = .{};
        const snapshot_err = ceu.ra8_ceu_status_snapshot(&status);
        if (snapshot_err != err.ok) return snapshot_err;
        state.last_events |= status.events;
        if (policy.isFatal(status.events)) {
            _ = ceu.ra8_ceu_clear_status(status.events);
            const reset_err = resetAndClear();
            if (reset_err != err.ok) return reset_err;
            return hw.error_;
        }
        if (policy.isComplete(status.events)) {
            out_bytes.* = policy.frameBytes(
                state.info.frame_bytes_max,
                state.capture_format,
                status.data_size,
            );
            _ = ceu.ra8_ceu_clear_status(state.last_events);
            return err.ok;
        }
        ra8_delay_ms(state.poll_interval_ms);
    }
    const reset_err = resetAndClear();
    if (reset_err != err.ok) return reset_err;
    return hw.timeout;
}

/// Capture one frame into caller-owned storage: cache maintenance, arm, wait,
/// then publish a view of the bytes the peripheral actually wrote.
fn captureFrame(state: *State, buffer: *const core.Buffer, out_frame: *core.Frame) u16 {
    if (!state.initialized) return err.not_initialized;
    const data = buffer.data orelse return err.null_ptr;
    switch (policy.captureEntryFault(buffer.capacity, state.info.frame_bytes_max, @intFromPtr(data))) {
        .ok => {},
        .capacity_short => return err.invalid_size,
        .misaligned => return err.invalid_arg,
    }
    state.last_events = 0;
    const clean_err = ra8_cache_dcache_clean_invalidate_by_addr(data, state.info.frame_bytes_max);
    if (clean_err != err.ok) return clean_err;
    const buffers: ceu.Buffers = .{ .y_top = data };
    const arm_err = ceu.ra8_ceu_capture_start_ex(&buffers);
    if (arm_err != err.ok) return arm_err;
    var captured_bytes: u32 = 0;
    const wait_err = waitForFrame(state, &captured_bytes);
    if (wait_err != err.ok) return wait_err;
    switch (policy.capturedBytesFault(captured_bytes, buffer.capacity)) {
        .ok => {},
        .zero, .exceeds_capacity => return err.invalid_size,
    }
    const invalidate_err = ra8_cache_dcache_invalidate_by_addr(data, captured_bytes);
    if (invalidate_err != err.ok) return invalidate_err;
    out_frame.* = .{
        .data = data,
        .bytes = captured_bytes,
        .stride_bytes = state.info.stride_bytes,
        .width = state.info.width,
        .height = state.info.height,
        .format = state.info.format,
    };
    return err.ok;
}

/// Source vtable row: report the configured capture metadata.
fn getInfo(ctx: ?*anyopaque, out_info: ?*core.Info) callconv(.c) u16 {
    const raw = ctx orelse return err.not_initialized;
    const info = out_info orelse return err.null_ptr;
    const state: *const State = @ptrCast(@alignCast(raw));
    if (!state.initialized) return err.not_initialized;
    info.* = state.info;
    return err.ok;
}

/// Source vtable row: capture one frame.
fn capture(ctx: ?*anyopaque, buffer: ?*const core.Buffer, out_frame: ?*core.Frame) callconv(.c) u16 {
    const raw = ctx orelse return err.not_initialized;
    const span = buffer orelse return err.null_ptr;
    const out = out_frame orelse return err.null_ptr;
    const state: *State = @ptrCast(@alignCast(raw));
    return captureFrame(state, span, out);
}

/// Source vtable row: release the CEU claim taken at init and mark the backend
/// closed, so a later init starts from Closed exactly as a cold boot does.
/// State is cleared only once the HAL confirms the release, so a refused
/// deinit leaves a still-valid source rather than a handle pointing at a
/// peripheral nobody owns.
fn stop(ctx: ?*anyopaque) callconv(.c) u16 {
    const raw = ctx orelse return err.not_initialized;
    const state: *State = @ptrCast(@alignCast(raw));
    if (!state.initialized) return err.not_initialized;
    const deinit_err = ceu.ra8_ceu_deinit();
    if (deinit_err != err.ok) return deinit_err;
    state.* = .{};
    return err.ok;
}

/// CEU source vtable.
pub const iface: abi.SourceIface = .{ .get_info = getInfo, .capture = capture, .stop = stop };

/// Configuration faults to `ra8_err_t`: every one of them is an invalid
/// argument, as the C reported them.
fn cfgErr(fault: policy.CfgFault) u16 {
    return switch (fault) {
        .ok => err.ok,
        else => err.invalid_arg,
    };
}

pub export fn ra8_camera_source_ceu_init(
    source: ?*abi.Source,
    state: ?*State,
    cfg: ?*const Cfg,
) callconv(.c) u16 {
    const handle = source orelse return err.null_ptr;
    const backend = state orelse return err.null_ptr;
    const config = cfg orelse return err.null_ptr;
    handle.* = .{};
    backend.* = .{};
    const fault = cfgErr(policy.validateCfg(.{
        .frame_bytes_max = config.output.frame_bytes_max,
        .stride_bytes = config.output.stride_bytes,
        .width = config.output.width,
        .height = config.output.height,
        .output_format = config.output.format,
        .poll_interval_ms = config.poll_interval_ms,
        .poll_attempts = config.poll_attempts,
        .ceu_capture_format = config.ceu.capture_format,
        .image_area_size = config.ceu.image_area_size,
    }, core.format.jpeg));
    if (fault != err.ok) return fault;
    const init_err = ceu.ra8_ceu_init(&config.ceu);
    if (init_err != err.ok) return init_err;
    backend.* = .{
        .info = config.output,
        .capture_format = config.ceu.capture_format,
        .poll_interval_ms = config.poll_interval_ms,
        .poll_attempts = config.poll_attempts,
        .initialized = true,
    };
    handle.iface = &iface;
    handle.ctx = backend;
    return err.ok;
}

pub export fn ra8_camera_source_ceu_get_last_events(
    state: ?*const State,
    out_events: ?*u32,
) callconv(.c) u16 {
    const backend = state orelse return err.null_ptr;
    const events = out_events orelse return err.null_ptr;
    if (!backend.initialized) {
        events.* = 0;
        return err.not_initialized;
    }
    events.* = backend.last_events;
    return err.ok;
}

/// White-box seam: the bounded completion poll, for the host suites that drive
/// it against the fake CEU register window. Declared in
/// `src/ra8_camera_source_ceu_private.h`.
pub export fn priv_cam_ceu_wait_for_frame(state: ?*State, out_bytes: ?*u32) callconv(.c) u16 {
    const backend = state orelse return err.not_initialized;
    const bytes = out_bytes orelse return err.null_ptr;
    return waitForFrame(backend, bytes);
}

/// White-box seam: the capture entry, bypassing the facade's own guards.
/// Declared in `src/ra8_camera_source_ceu_private.h`.
pub export fn priv_cam_ceu_capture(
    state: ?*State,
    buffer: ?*const core.Buffer,
    out_frame: ?*core.Frame,
) callconv(.c) u16 {
    return capture(state, buffer, out_frame);
}
