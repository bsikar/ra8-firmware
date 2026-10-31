//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for the OV5640 open seam's ordering and fault policy. The
//! sensor driver and the CEU capture backend are satisfied here by exported
//! mock symbols, so the suite exercises exactly what the seam owns: the
//! probe -> configure -> CEU bind order, argument passthrough, and stopping
//! at the first failure with the CEU unclaimed.

const std = @import("std");
const open_mod = @import("ov5640_open");

const Source = open_mod.Source;
const err = open_mod.err;

const Step = enum { probe, configure, init };
var calls: [8]Step = undefined;
var call_count: usize = 0;
var probe_err: u16 = 0;
var configure_err: u16 = 0;
var init_err: u16 = 0;
var configured_mode: u8 = 0xAA;
var probe_dev: usize = 0;
var init_state: usize = 0;
var init_cfg: usize = 0;

fn reset() void {
    call_count = 0;
    probe_err = 0;
    configure_err = 0;
    init_err = 0;
    configured_mode = 0xAA;
    probe_dev = 0;
    init_state = 0;
    init_cfg = 0;
}

fn log(step: Step) void {
    calls[call_count] = step;
    call_count += 1;
}

export fn ra8_ov5640_probe(dev: ?*anyopaque, out_id: ?*u16) callconv(.c) u16 {
    log(.probe);
    probe_dev = @intFromPtr(dev);
    if (out_id) |id| id.* = 0x5640;
    return probe_err;
}

export fn ra8_ov5640_configure(dev: ?*anyopaque, mode: open_mod.Mode) callconv(.c) u16 {
    _ = dev;
    log(.configure);
    configured_mode = mode;
    return configure_err;
}

export fn ra8_camera_source_ceu_init(source: ?*Source, state: ?*anyopaque, cfg: ?*const anyopaque) callconv(.c) u16 {
    _ = source;
    log(.init);
    init_state = @intFromPtr(state);
    init_cfg = @intFromPtr(cfg);
    return init_err;
}

test "open runs probe, configure, then CEU bind in order and passes arguments through" {
    reset();
    var handle: Source = .{};
    var state: [64]u8 = @splat(0);
    var sensor: [16]u8 = @splat(0);
    var cfg: [128]u8 = @splat(0);
    const rc = open_mod.ra8_camera_ov5640_open(&handle, &state, &sensor, 1, &cfg);
    try std.testing.expectEqual(err.ok, rc);
    try std.testing.expectEqualSlices(Step, &.{ .probe, .configure, .init }, calls[0..call_count]);
    try std.testing.expectEqual(@as(u8, 1), configured_mode);
    try std.testing.expectEqual(@intFromPtr(&sensor), probe_dev);
    try std.testing.expectEqual(@intFromPtr(&state), init_state);
    try std.testing.expectEqual(@intFromPtr(&cfg), init_cfg);
}

test "open forwards a probe fault and leaves the CEU unclaimed" {
    reset();
    probe_err = 0x2A0;
    var handle: Source = .{};
    var state: [64]u8 = @splat(0);
    var sensor: [16]u8 = @splat(0);
    var cfg: [128]u8 = @splat(0);
    const rc = open_mod.ra8_camera_ov5640_open(&handle, &state, &sensor, 0, &cfg);
    try std.testing.expectEqual(@as(u16, 0x2A0), rc);
    try std.testing.expectEqualSlices(Step, &.{.probe}, calls[0..call_count]);
}

test "open forwards a configure fault and leaves the CEU unclaimed" {
    reset();
    configure_err = 0x2A1;
    var handle: Source = .{};
    var state: [64]u8 = @splat(0);
    var sensor: [16]u8 = @splat(0);
    var cfg: [128]u8 = @splat(0);
    const rc = open_mod.ra8_camera_ov5640_open(&handle, &state, &sensor, 0, &cfg);
    try std.testing.expectEqual(@as(u16, 0x2A1), rc);
    try std.testing.expectEqualSlices(Step, &.{ .probe, .configure }, calls[0..call_count]);
}

test "open forwards a CEU bind fault after a full sensor bring-up" {
    reset();
    init_err = 0x2A2;
    var handle: Source = .{};
    var state: [64]u8 = @splat(0);
    var sensor: [16]u8 = @splat(0);
    var cfg: [128]u8 = @splat(0);
    const rc = open_mod.ra8_camera_ov5640_open(&handle, &state, &sensor, 0, &cfg);
    try std.testing.expectEqual(@as(u16, 0x2A2), rc);
    try std.testing.expectEqualSlices(Step, &.{ .probe, .configure, .init }, calls[0..call_count]);
}

test "open rejects null arguments before any hardware call" {
    reset();
    var handle: Source = .{};
    var state: [64]u8 = @splat(0);
    var sensor: [16]u8 = @splat(0);
    var cfg: [128]u8 = @splat(0);
    const cases = [_]u16{
        open_mod.ra8_camera_ov5640_open(null, &state, &sensor, 0, &cfg),
        open_mod.ra8_camera_ov5640_open(&handle, null, &sensor, 0, &cfg),
        open_mod.ra8_camera_ov5640_open(&handle, &state, null, 0, &cfg),
        open_mod.ra8_camera_ov5640_open(&handle, &state, &sensor, 0, null),
    };
    for (cases) |rc| {
        try std.testing.expectEqual(err.null_ptr, rc);
    }
    try std.testing.expectEqual(@as(usize, 0), call_count);
}
