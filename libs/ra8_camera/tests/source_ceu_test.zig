//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for the CEU capture-source backend and its policy. The CEU HAL,
//! the cache maintenance calls and the millisecond delay are satisfied here by
//! a modelled peripheral: the test binary exports the same C symbols the
//! archive calls, which is the link-time equivalent of the `#define`
//! interposition the C coverage suite used to do around a white-box copy.

const std = @import("std");
const source = @import("source_ceu");

const policy = source.policy;
const err = source.err;
const hw_timeout: u16 = 0x203;
const hw_error: u16 = 0x204;
const hw_unmapped: u16 = 0x209;

/// Modelled CEU peripheral. One flat block of state, reset before each case.
const model = struct {
    var events_by_poll: [8]u32 = .{0} ** 8;
    var data_size: u32 = 0;
    var polls: u32 = 0;
    var status_err: u16 = 0;
    var reset_err: u16 = 0;
    var init_err: u16 = 0;
    var deinit_err: u16 = 0;
    var arm_err: u16 = 0;
    var clean_err: u16 = 0;
    var invalidate_err: u16 = 0;
    var cleared: u32 = 0;
    var resets: u32 = 0;
    var arms: u32 = 0;
    var delays: u32 = 0;
    var armed_address: usize = 0;

    fn reset() void {
        events_by_poll = .{0} ** 8;
        data_size = 0;
        polls = 0;
        status_err = 0;
        reset_err = 0;
        init_err = 0;
        deinit_err = 0;
        arm_err = 0;
        clean_err = 0;
        invalidate_err = 0;
        cleared = 0;
        resets = 0;
        arms = 0;
        delays = 0;
        armed_address = 0;
    }

    /// Every poll observes the same event bits unless a case staged a sequence.
    fn stage(events: u32) void {
        events_by_poll = .{events} ** 8;
    }
};

export fn ra8_ceu_status_snapshot(out_status: *source.Status) callconv(.c) u16 {
    if (model.status_err != 0) return model.status_err;
    const index = @min(model.polls, model.events_by_poll.len - 1);
    out_status.* = .{ .events = model.events_by_poll[index], .data_size = model.data_size };
    model.polls += 1;
    return err.ok;
}

export fn ra8_ceu_clear_status(event_bits: u32) callconv(.c) u16 {
    model.cleared |= event_bits;
    return err.ok;
}

export fn ra8_ceu_reset() callconv(.c) u16 {
    model.resets += 1;
    return model.reset_err;
}

export fn ra8_ceu_init(cfg: *const source.CeuConfig) callconv(.c) u16 {
    _ = cfg;
    return model.init_err;
}

export fn ra8_ceu_deinit() callconv(.c) u16 {
    return model.deinit_err;
}

export fn ra8_ceu_capture_start_ex(buffers: *const source.CeuBuffers) callconv(.c) u16 {
    model.arms += 1;
    model.armed_address = @intFromPtr(buffers.y_top orelse return err.null_ptr);
    return model.arm_err;
}

export fn ra8_cache_dcache_clean_invalidate_by_addr(addr: ?*const anyopaque, size: u32) callconv(.c) u16 {
    _ = addr;
    _ = size;
    return model.clean_err;
}

export fn ra8_cache_dcache_invalidate_by_addr(addr: ?*const anyopaque, size: u32) callconv(.c) u16 {
    _ = addr;
    _ = size;
    return model.invalidate_err;
}

export fn ra8_delay_ms(ms: u32) callconv(.c) void {
    _ = ms;
    model.delays += 1;
}

/// Bare backend state with no CEU claim behind it, the shape the poll-loop
/// cases drive.
fn bareState(frame_bytes: u32, capture_format: u8, attempts: u32) source.State {
    return .{
        .info = .{ .frame_bytes_max = frame_bytes, .stride_bytes = 32, .width = 16, .height = 16, .format = 1 },
        .capture_format = capture_format,
        .poll_interval_ms = 1,
        .poll_attempts = attempts,
        .initialized = true,
    };
}

/// An accepted UYVY configuration; each case mutates the one field it is about.
fn acceptedCfg() source.Cfg {
    return .{
        .ceu = .{ .capture_format = policy.capture_format.image_capture },
        .output = .{ .frame_bytes_max = 512, .stride_bytes = 32, .width = 16, .height = 16, .format = 1 },
        .poll_interval_ms = 1,
        .poll_attempts = 4,
    };
}

var capture_storage: [1024]u8 align(8) = undefined;

test "frameBytes: fixed frame reports the configured bound, CDSSR ignored" {
    // MC/DC vector 1: outer condition false, so the latched 200 is not used.
    try std.testing.expectEqual(@as(u32, 128), policy.frameBytes(128, policy.capture_format.image_capture, 200));
}

test "frameBytes: data-enable reports CDSSR when the peripheral latched one" {
    // MC/DC vector 2: outer true, inner true. Flips the outcome against
    // vector 1 by varying the capture format only.
    try std.testing.expectEqual(@as(u32, 200), policy.frameBytes(128, policy.capture_format.data_enable, 200));
}

test "frameBytes: data-enable with CDSSR zero falls back to the bound" {
    // MC/DC vector 3: outer true, inner false. Flips the outcome against
    // vector 2 by varying CDSSR only.
    try std.testing.expectEqual(@as(u32, 128), policy.frameBytes(128, policy.capture_format.data_enable, 0));
}

test "event classification: faults are fatal, sync traffic and completion are not" {
    try std.testing.expect(policy.isFatal(policy.events.igrw));
    try std.testing.expect(policy.isFatal(policy.events.cram_overflow));
    try std.testing.expect(policy.isFatal(policy.events.vd_error));
    try std.testing.expect(policy.isFatal(policy.events.firewall));
    try std.testing.expect(!policy.isFatal(policy.events.hd | policy.events.vd));
    try std.testing.expect(!policy.isFatal(policy.events.cpe));
    try std.testing.expect(policy.isComplete(policy.events.cpe | policy.events.hd));
    try std.testing.expect(!policy.isComplete(policy.events.hd));
}

test "wait: a completed frame reports its bytes and clears what it observed" {
    model.reset();
    model.stage(policy.events.cpe);
    model.data_size = 64;
    var state = bareState(128, policy.capture_format.image_capture, 4);
    var bytes: u32 = 0;
    try std.testing.expectEqual(err.ok, source.priv_cam_ceu_wait_for_frame(&state, &bytes));
    try std.testing.expectEqual(@as(u32, 128), bytes);
    try std.testing.expectEqual(policy.events.cpe, state.last_events);
    try std.testing.expectEqual(policy.events.cpe, model.cleared);
    try std.testing.expectEqual(@as(u32, 0), model.resets);
}

test "wait: a fatal fault abandons the frame even alongside completion" {
    // The HUM-documented rare case: CPE asserted despite an invalid vertical
    // blanking interval. Both bits stay readable in diagnostic state.
    model.reset();
    const observed = policy.events.vd_error | policy.events.cpe;
    model.stage(observed);
    var state = bareState(128, policy.capture_format.data_enable, 1);
    var bytes: u32 = 0;
    try std.testing.expectEqual(hw_error, source.priv_cam_ceu_wait_for_frame(&state, &bytes));
    try std.testing.expectEqual(@as(u32, 0), bytes);
    try std.testing.expectEqual(observed, state.last_events);
    try std.testing.expectEqual(@as(u32, 1), model.resets);
}

test "wait: sync traffic alone never terminates the bounded poll" {
    model.reset();
    const sync = policy.events.hd | policy.events.vd;
    model.stage(sync);
    var state = bareState(128, policy.capture_format.image_capture, 3);
    var bytes: u32 = 0;
    try std.testing.expectEqual(hw_timeout, source.priv_cam_ceu_wait_for_frame(&state, &bytes));
    try std.testing.expectEqual(sync, state.last_events);
    try std.testing.expectEqual(@as(u32, 3), model.delays);
    try std.testing.expectEqual(@as(u32, 1), model.resets);
}

test "wait: completion on a later poll still reports the frame" {
    model.reset();
    model.events_by_poll = .{ policy.events.hd, policy.events.hd, policy.events.cpe } ++ .{0} ** 5;
    var state = bareState(256, policy.capture_format.image_capture, 8);
    var bytes: u32 = 0;
    try std.testing.expectEqual(err.ok, source.priv_cam_ceu_wait_for_frame(&state, &bytes));
    try std.testing.expectEqual(@as(u32, 256), bytes);
    try std.testing.expectEqual(policy.events.hd | policy.events.cpe, state.last_events);
}

test "wait: a failed status read is forwarded before any event is recorded" {
    model.reset();
    model.status_err = hw_unmapped;
    var state = bareState(128, policy.capture_format.image_capture, 4);
    var bytes: u32 = 0;
    try std.testing.expectEqual(hw_unmapped, source.priv_cam_ceu_wait_for_frame(&state, &bytes));
    try std.testing.expectEqual(@as(u32, 0), state.last_events);
    try std.testing.expectEqual(@as(u32, 0), model.resets);
}

test "wait: a failed software reset is reported in place of the fault it cleared" {
    model.reset();
    model.stage(policy.events.firewall);
    model.reset_err = hw_unmapped;
    var state = bareState(128, policy.capture_format.image_capture, 2);
    var bytes: u32 = 0;
    try std.testing.expectEqual(hw_unmapped, source.priv_cam_ceu_wait_for_frame(&state, &bytes));
    try std.testing.expectEqual(policy.events.firewall, state.last_events);
}

test "wait: a failed software reset substitutes on the expiry leg too" {
    model.reset();
    model.reset_err = hw_unmapped;
    var state = bareState(128, policy.capture_format.image_capture, 1);
    var bytes: u32 = 0;
    try std.testing.expectEqual(hw_unmapped, source.priv_cam_ceu_wait_for_frame(&state, &bytes));
    try std.testing.expectEqual(@as(u32, 0), state.last_events);
}

test "capture: a completed capture publishes a frame aliasing the caller's buffer" {
    model.reset();
    model.stage(policy.events.cpe);
    var state = bareState(128, policy.capture_format.image_capture, 4);
    const buffer: source.Buffer = .{ .data = &capture_storage, .capacity = 512 };
    var frame: source.Frame = .{};
    try std.testing.expectEqual(err.ok, source.priv_cam_ceu_capture(&state, &buffer, &frame));
    try std.testing.expectEqual(@intFromPtr(&capture_storage), @intFromPtr(frame.data.?));
    try std.testing.expectEqual(@as(u32, 128), frame.bytes);
    try std.testing.expectEqual(@as(u32, 32), frame.stride_bytes);
    try std.testing.expectEqual(@as(u16, 16), frame.width);
    try std.testing.expectEqual(@as(u16, 16), frame.height);
    try std.testing.expectEqual(@as(u8, 1), frame.format);
    try std.testing.expectEqual(@intFromPtr(&capture_storage), model.armed_address);
}

test "capture: a capture that produced nothing cannot be published" {
    // MC/DC vector 1 of the published-byte-count guard: zero bytes.
    model.reset();
    model.stage(policy.events.cpe);
    var state = bareState(0, policy.capture_format.image_capture, 4);
    const buffer: source.Buffer = .{ .data = &capture_storage, .capacity = 512 };
    var frame: source.Frame = .{};
    try std.testing.expectEqual(err.invalid_size, source.priv_cam_ceu_capture(&state, &buffer, &frame));
    try std.testing.expect(frame.data == null);
}

test "capture: a data-enable capture larger than the storage is refused" {
    // MC/DC vector 2 of the same guard: over capacity, staged through CDSSR.
    model.reset();
    model.stage(policy.events.cpe);
    model.data_size = 4096;
    var state = bareState(128, policy.capture_format.data_enable, 4);
    const buffer: source.Buffer = .{ .data = &capture_storage, .capacity = 512 };
    var frame: source.Frame = .{};
    try std.testing.expectEqual(err.invalid_size, source.priv_cam_ceu_capture(&state, &buffer, &frame));
    try std.testing.expect(frame.data == null);
}

test "capture: entry guards reject short storage and a misaligned address" {
    try std.testing.expectEqual(policy.EntryFault.capacity_short, policy.captureEntryFault(64, 128, 0x68000040));
    try std.testing.expectEqual(policy.EntryFault.misaligned, policy.captureEntryFault(256, 128, 0x68000041));
    try std.testing.expectEqual(policy.EntryFault.ok, policy.captureEntryFault(256, 128, 0x68000040));

    model.reset();
    var state = bareState(128, policy.capture_format.image_capture, 4);
    const short: source.Buffer = .{ .data = &capture_storage, .capacity = 64 };
    var frame: source.Frame = .{};
    try std.testing.expectEqual(err.invalid_size, source.priv_cam_ceu_capture(&state, &short, &frame));
    const misaligned: source.Buffer = .{ .data = @ptrCast(capture_storage[1..]), .capacity = 512 };
    try std.testing.expectEqual(err.invalid_arg, source.priv_cam_ceu_capture(&state, &misaligned, &frame));
    try std.testing.expectEqual(@as(u32, 0), model.arms);
}

test "capture: cache and arming failures are forwarded untouched" {
    model.reset();
    model.clean_err = hw_unmapped;
    var state = bareState(128, policy.capture_format.image_capture, 4);
    const buffer: source.Buffer = .{ .data = &capture_storage, .capacity = 512 };
    var frame: source.Frame = .{};
    try std.testing.expectEqual(hw_unmapped, source.priv_cam_ceu_capture(&state, &buffer, &frame));
    try std.testing.expectEqual(@as(u32, 0), model.arms);

    model.reset();
    model.arm_err = hw_error;
    try std.testing.expectEqual(hw_error, source.priv_cam_ceu_capture(&state, &buffer, &frame));

    model.reset();
    model.stage(policy.events.cpe);
    model.invalidate_err = hw_unmapped;
    try std.testing.expectEqual(hw_unmapped, source.priv_cam_ceu_capture(&state, &buffer, &frame));
}

test "capture: an uninitialized or absent backend is not initialized" {
    model.reset();
    var closed: source.State = .{};
    const buffer: source.Buffer = .{ .data = &capture_storage, .capacity = 512 };
    var frame: source.Frame = .{};
    try std.testing.expectEqual(err.not_initialized, source.priv_cam_ceu_capture(&closed, &buffer, &frame));
    try std.testing.expectEqual(err.not_initialized, source.priv_cam_ceu_capture(null, &buffer, &frame));
    try std.testing.expectEqual(err.null_ptr, source.priv_cam_ceu_capture(&closed, null, &frame));
}

test "validateCfg: every bound rejection in the order the backend applies them" {
    const jpeg = source.core.format.jpeg;
    var view: policy.CfgView = .{
        .frame_bytes_max = 512,
        .stride_bytes = 32,
        .width = 16,
        .height = 16,
        .output_format = 1,
        .poll_interval_ms = 1,
        .poll_attempts = 4,
        .ceu_capture_format = policy.capture_format.image_capture,
        .image_area_size = 0,
    };
    try std.testing.expectEqual(policy.CfgFault.ok, policy.validateCfg(view, jpeg));

    view.frame_bytes_max = 0;
    try std.testing.expectEqual(policy.CfgFault.zero_frame_bytes, policy.validateCfg(view, jpeg));
    view.frame_bytes_max = 512;
    view.width = 0;
    try std.testing.expectEqual(policy.CfgFault.zero_width, policy.validateCfg(view, jpeg));
    view.width = 16;
    view.height = 0;
    try std.testing.expectEqual(policy.CfgFault.zero_height, policy.validateCfg(view, jpeg));
    view.height = 16;
    view.poll_interval_ms = 0;
    try std.testing.expectEqual(policy.CfgFault.zero_poll_interval, policy.validateCfg(view, jpeg));
    view.poll_interval_ms = 1;
    view.poll_attempts = 0;
    try std.testing.expectEqual(policy.CfgFault.zero_poll_attempts, policy.validateCfg(view, jpeg));
}

test "validateCfg: both halves of the format pairing and the JPEG-only rules" {
    const jpeg = source.core.format.jpeg;
    var view: policy.CfgView = .{
        .frame_bytes_max = 512,
        .stride_bytes = 0,
        .width = 16,
        .height = 16,
        .output_format = jpeg,
        .poll_interval_ms = 1,
        .poll_attempts = 4,
        .ceu_capture_format = policy.capture_format.data_enable,
        .image_area_size = 512,
    };
    // Both conditions agree: data-enable framing with a JPEG consumer.
    try std.testing.expectEqual(policy.CfgFault.ok, policy.validateCfg(view, jpeg));
    // MC/DC vector 2: JPEG output without data-enable framing.
    view.ceu_capture_format = policy.capture_format.image_capture;
    try std.testing.expectEqual(policy.CfgFault.format_pairing, policy.validateCfg(view, jpeg));
    // MC/DC vector 3: data-enable framing with a raw output format.
    view.ceu_capture_format = policy.capture_format.data_enable;
    view.output_format = 1;
    try std.testing.expectEqual(policy.CfgFault.format_pairing, policy.validateCfg(view, jpeg));
    // JPEG-only: a row stride is meaningless for a compressed frame.
    view.output_format = jpeg;
    view.stride_bytes = 32;
    try std.testing.expectEqual(policy.CfgFault.jpeg_stride_set, policy.validateCfg(view, jpeg));
    // JPEG-only: the firewall window must match the declared capacity.
    view.stride_bytes = 0;
    view.image_area_size = 256;
    try std.testing.expectEqual(policy.CfgFault.jpeg_area_mismatch, policy.validateCfg(view, jpeg));
}

test "init: null guards, then a rejected configuration leaves nothing bound" {
    model.reset();
    var handle: source.Source = .{};
    var state: source.State = .{};
    var cfg = acceptedCfg();
    try std.testing.expectEqual(err.null_ptr, source.ra8_camera_source_ceu_init(null, &state, &cfg));
    try std.testing.expectEqual(err.null_ptr, source.ra8_camera_source_ceu_init(&handle, null, &cfg));
    try std.testing.expectEqual(err.null_ptr, source.ra8_camera_source_ceu_init(&handle, &state, null));

    cfg.poll_attempts = 0;
    try std.testing.expectEqual(err.invalid_arg, source.ra8_camera_source_ceu_init(&handle, &state, &cfg));
    try std.testing.expect(handle.iface == null);
    try std.testing.expect(!state.initialized);
}

test "init: a refused CEU claim binds nothing, an accepted one binds the vtable" {
    model.reset();
    model.init_err = hw_error;
    var handle: source.Source = .{};
    var state: source.State = .{};
    const cfg = acceptedCfg();
    try std.testing.expectEqual(hw_error, source.ra8_camera_source_ceu_init(&handle, &state, &cfg));
    try std.testing.expect(handle.iface == null);
    try std.testing.expect(!state.initialized);

    model.init_err = 0;
    try std.testing.expectEqual(err.ok, source.ra8_camera_source_ceu_init(&handle, &state, &cfg));
    try std.testing.expect(handle.iface == &source.iface);
    try std.testing.expectEqual(@intFromPtr(&state), @intFromPtr(handle.ctx.?));
    try std.testing.expect(state.initialized);
    try std.testing.expectEqual(@as(u32, 512), state.info.frame_bytes_max);
    try std.testing.expectEqual(policy.capture_format.image_capture, state.capture_format);
}

test "get_last_events: guards, the unbound zero, and the retained snapshot" {
    model.reset();
    var events: u32 = 0xFFFF;
    var state: source.State = .{};
    try std.testing.expectEqual(err.null_ptr, source.ra8_camera_source_ceu_get_last_events(null, &events));
    try std.testing.expectEqual(err.null_ptr, source.ra8_camera_source_ceu_get_last_events(&state, null));
    try std.testing.expectEqual(err.not_initialized, source.ra8_camera_source_ceu_get_last_events(&state, &events));
    try std.testing.expectEqual(@as(u32, 0), events);

    state = bareState(128, policy.capture_format.image_capture, 4);
    state.last_events = policy.events.cpe | policy.events.hd;
    try std.testing.expectEqual(err.ok, source.ra8_camera_source_ceu_get_last_events(&state, &events));
    try std.testing.expectEqual(policy.events.cpe | policy.events.hd, events);
}

test "vtable: get_info copies metadata and refuses an unbound backend" {
    model.reset();
    var state = bareState(128, policy.capture_format.image_capture, 4);
    var info: source.Info = .{};
    const get_info = source.iface.get_info.?;
    try std.testing.expectEqual(err.ok, get_info(&state, &info));
    try std.testing.expectEqual(@as(u32, 128), info.frame_bytes_max);
    try std.testing.expectEqual(@as(u16, 16), info.width);
    try std.testing.expectEqual(err.not_initialized, get_info(null, &info));
    try std.testing.expectEqual(err.null_ptr, get_info(&state, null));

    var closed: source.State = .{};
    try std.testing.expectEqual(err.not_initialized, get_info(&closed, &info));
}

test "vtable: stop releases the claim, and a refused deinit leaves state valid" {
    model.reset();
    model.deinit_err = hw_error;
    var state = bareState(128, policy.capture_format.image_capture, 4);
    const stop = source.iface.stop.?;
    try std.testing.expectEqual(hw_error, stop(&state));
    try std.testing.expect(state.initialized);
    try std.testing.expectEqual(@as(u32, 128), state.info.frame_bytes_max);

    model.deinit_err = 0;
    try std.testing.expectEqual(err.ok, stop(&state));
    try std.testing.expect(!state.initialized);
    try std.testing.expectEqual(@as(u32, 0), state.info.frame_bytes_max);
    try std.testing.expectEqual(err.not_initialized, stop(&state));
    try std.testing.expectEqual(err.not_initialized, stop(null));
}
