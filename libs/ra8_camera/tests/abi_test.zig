//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Tests for the C ABI membrane in `src/ra8_camera_abi.zig`: the guard order
//! of all four exported symbols, the `ra8_err_t` each fault maps to, and the
//! post-dispatch contract checks a misbehaving backend can violate. The
//! injected vtables mirror the fault-injecting rows in
//! `tests/misc/src/test_ra8_camera.c` and `test_ra8_camera_contract.c`.

const std = @import("std");
const abi = @import("facade");

const core = abi.core;
const err = abi.err;
const rgb888 = core.format.rgb888;
const jpeg = core.format.jpeg;

/// The injected response one fault row publishes, plus the call counter that
/// proves whether the facade rejected before dispatch or after.
const Fault = struct {
    frame: core.Frame = .{},
    result: u16 = err.ok,
    calls: u32 = 0,
    info_calls: u32 = 0,
};

var fault: Fault = .{};
var pixels = [_]u8{0} ** 64;

fn faultGetInfo(ctx: ?*anyopaque, out_info: ?*core.Info) callconv(.c) u16 {
    _ = ctx;
    fault.info_calls += 1;
    out_info.?.* = .{
        .frame_bytes_max = 3,
        .stride_bytes = 3,
        .width = 1,
        .height = 1,
        .format = rgb888,
    };
    return err.ok;
}

fn faultCapture(ctx: ?*anyopaque, buffer: ?*const core.Buffer, out_frame: ?*core.Frame) callconv(.c) u16 {
    _ = ctx;
    _ = buffer;
    fault.calls += 1;
    out_frame.?.* = fault.frame;
    return fault.result;
}

fn faultEncode(
    ctx: ?*anyopaque,
    input: ?*const core.Frame,
    output_buffer: ?*const core.Buffer,
    out_frame: ?*core.Frame,
) callconv(.c) u16 {
    _ = ctx;
    _ = input;
    _ = output_buffer;
    fault.calls += 1;
    out_frame.?.* = fault.frame;
    return fault.result;
}

const full_source: abi.SourceIface = .{ .get_info = faultGetInfo, .capture = faultCapture };
const no_get_info: abi.SourceIface = .{ .get_info = null, .capture = faultCapture };
const no_capture: abi.SourceIface = .{ .get_info = faultGetInfo, .capture = null };
const full_codec: abi.CodecIface = .{ .encode = faultEncode };
const no_encode: abi.CodecIface = .{ .encode = null };

fn reset() void {
    fault = .{};
}

fn validFrame() core.Frame {
    return .{
        .data = &pixels,
        .bytes = 3,
        .stride_bytes = 3,
        .width = 1,
        .height = 1,
        .format = rgb888,
    };
}

test "frame_validate: a null handle is null_ptr and a good frame is ok" {
    try std.testing.expectEqual(err.null_ptr, abi.ra8_camera_frame_validate(null));
    var frame = validFrame();
    try std.testing.expectEqual(err.ok, abi.ra8_camera_frame_validate(&frame));
}

test "frame_validate: every fault maps to its documented ra8_err_t" {
    var frame = validFrame();
    frame.data = null;
    try std.testing.expectEqual(err.null_ptr, abi.ra8_camera_frame_validate(&frame));
    frame = validFrame();
    frame.width = 0;
    try std.testing.expectEqual(err.invalid_arg, abi.ra8_camera_frame_validate(&frame));
    frame = validFrame();
    frame.height = 0;
    try std.testing.expectEqual(err.invalid_arg, abi.ra8_camera_frame_validate(&frame));
    frame = validFrame();
    frame.format = 99;
    try std.testing.expectEqual(err.invalid_arg, abi.ra8_camera_frame_validate(&frame));
    frame = validFrame();
    frame.stride_bytes = 2;
    try std.testing.expectEqual(err.invalid_size, abi.ra8_camera_frame_validate(&frame));
    frame = validFrame();
    frame.bytes = 2;
    try std.testing.expectEqual(err.invalid_size, abi.ra8_camera_frame_validate(&frame));
}

test "frame_validate: a jpeg view needs bytes and no stride" {
    var frame = core.Frame{ .data = &pixels, .bytes = 2, .width = 1, .height = 1, .format = jpeg };
    try std.testing.expectEqual(err.ok, abi.ra8_camera_frame_validate(&frame));
    frame.bytes = 0;
    try std.testing.expectEqual(err.invalid_size, abi.ra8_camera_frame_validate(&frame));
    frame.bytes = 2;
    frame.stride_bytes = 1;
    try std.testing.expectEqual(err.invalid_size, abi.ra8_camera_frame_validate(&frame));
}

test "frame_validate: an odd uyvy width is invalid_arg" {
    var frame = core.Frame{
        .data = &pixels,
        .bytes = 30,
        .stride_bytes = 30,
        .width = 15,
        .height = 1,
        .format = core.format.uyvy422,
    };
    try std.testing.expectEqual(err.invalid_arg, abi.ra8_camera_frame_validate(&frame));
}

test "get_info: the output pointer is judged before the handle" {
    reset();
    var source = abi.Source{};
    var info = core.Info{ .frame_bytes_max = 7 };
    try std.testing.expectEqual(err.null_ptr, abi.ra8_camera_source_get_info(&source, null));
    try std.testing.expectEqual(err.null_ptr, abi.ra8_camera_source_get_info(null, &info));
    // Even the null-handle rejection has already zeroed the caller's row.
    try std.testing.expectEqual(@as(u32, 0), info.frame_bytes_max);
}

test "get_info: an unbound or half-bound source is not_initialized" {
    reset();
    var unbound = abi.Source{};
    var info = core.Info{};
    try std.testing.expectEqual(err.not_initialized, abi.ra8_camera_source_get_info(&unbound, &info));
    var half = abi.Source{ .iface = &no_get_info };
    try std.testing.expectEqual(err.not_initialized, abi.ra8_camera_source_get_info(&half, &info));
    // A missing capture row also blocks get_info: both rows are mandatory.
    var other = abi.Source{ .iface = &no_capture };
    try std.testing.expectEqual(err.not_initialized, abi.ra8_camera_source_get_info(&other, &info));
    try std.testing.expectEqual(@as(u32, 0), fault.info_calls);
}

test "get_info: a bound source reaches its backend row" {
    reset();
    var source = abi.Source{ .iface = &full_source, .ctx = null };
    var info = core.Info{};
    try std.testing.expectEqual(err.ok, abi.ra8_camera_source_get_info(&source, &info));
    try std.testing.expectEqual(@as(u32, 1), fault.info_calls);
    try std.testing.expectEqual(@as(u32, 3), info.frame_bytes_max);
    try std.testing.expectEqual(@as(u16, 1), info.width);
}

test "capture: the buffer and output pointers are judged before the handle" {
    reset();
    var source = abi.Source{};
    var out = core.Frame{ .bytes = 9 };
    const buffer = core.Buffer{ .data = &pixels, .capacity = pixels.len };
    try std.testing.expectEqual(err.null_ptr, abi.ra8_camera_source_capture(&source, null, &out));
    try std.testing.expectEqual(err.null_ptr, abi.ra8_camera_source_capture(&source, &buffer, null));
    try std.testing.expectEqual(err.null_ptr, abi.ra8_camera_source_capture(null, &buffer, &out));
    try std.testing.expectEqual(@as(u32, 0), out.bytes);
}

test "capture: an unbound or half-bound source is not_initialized before the buffer guards" {
    reset();
    var out = core.Frame{};
    const no_data = core.Buffer{ .data = null, .capacity = 8 };
    var unbound = abi.Source{};
    // A NULL buffer pointer on an unbound source still answers not_initialized:
    // the handle is judged before the buffer contents.
    try std.testing.expectEqual(err.not_initialized, abi.ra8_camera_source_capture(&unbound, &no_data, &out));
    var half = abi.Source{ .iface = &no_capture };
    const buffer = core.Buffer{ .data = &pixels, .capacity = pixels.len };
    try std.testing.expectEqual(err.not_initialized, abi.ra8_camera_source_capture(&half, &buffer, &out));
    var other = abi.Source{ .iface = &no_get_info };
    try std.testing.expectEqual(err.not_initialized, abi.ra8_camera_source_capture(&other, &buffer, &out));
    try std.testing.expectEqual(@as(u32, 0), fault.calls);
}

test "capture: buffer contents are judged after the handle and before dispatch" {
    reset();
    var bound = abi.Source{ .iface = &full_source, .ctx = null };
    var out = core.Frame{};
    const no_data = core.Buffer{ .data = null, .capacity = 8 };
    try std.testing.expectEqual(err.null_ptr, abi.ra8_camera_source_capture(&bound, &no_data, &out));
    const empty = core.Buffer{ .data = &pixels, .capacity = 0 };
    try std.testing.expectEqual(err.invalid_size, abi.ra8_camera_source_capture(&bound, &empty, &out));
    try std.testing.expectEqual(@as(u32, 0), fault.calls);
}

test "capture: a backend error is forwarded and clears the output" {
    reset();
    fault.result = 0x109;
    fault.frame = validFrame();
    var bound = abi.Source{ .iface = &full_source, .ctx = null };
    var out = core.Frame{};
    const buffer = core.Buffer{ .data = &pixels, .capacity = pixels.len };
    try std.testing.expectEqual(@as(u16, 0x109), abi.ra8_camera_source_capture(&bound, &buffer, &out));
    try std.testing.expect(out.data == null);
    try std.testing.expectEqual(@as(u32, 1), fault.calls);
}

test "capture: a frame that does not alias the buffer is invalid_size" {
    reset();
    var elsewhere = [_]u8{0} ** 8;
    fault.frame = .{
        .data = &elsewhere,
        .bytes = 3,
        .stride_bytes = 3,
        .width = 1,
        .height = 1,
        .format = rgb888,
    };
    var bound = abi.Source{ .iface = &full_source, .ctx = null };
    var out = core.Frame{};
    const buffer = core.Buffer{ .data = &pixels, .capacity = pixels.len };
    try std.testing.expectEqual(err.invalid_size, abi.ra8_camera_source_capture(&bound, &buffer, &out));
    try std.testing.expect(out.data == null);
}

test "capture: a frame claiming more bytes than the buffer holds is invalid_size" {
    reset();
    fault.frame = .{
        .data = &pixels,
        .bytes = pixels.len + 1,
        .stride_bytes = 3,
        .width = 1,
        .height = 1,
        .format = rgb888,
    };
    var bound = abi.Source{ .iface = &full_source, .ctx = null };
    var out = core.Frame{};
    const buffer = core.Buffer{ .data = &pixels, .capacity = pixels.len };
    try std.testing.expectEqual(err.invalid_size, abi.ra8_camera_source_capture(&bound, &buffer, &out));
    try std.testing.expect(out.data == null);
}

test "capture: an aliased frame that fails validation is rejected and cleared" {
    reset();
    fault.frame = .{
        .data = &pixels,
        .bytes = 3,
        .stride_bytes = 3,
        .width = 0,
        .height = 1,
        .format = rgb888,
    };
    var bound = abi.Source{ .iface = &full_source, .ctx = null };
    var out = core.Frame{};
    const buffer = core.Buffer{ .data = &pixels, .capacity = pixels.len };
    try std.testing.expectEqual(err.invalid_arg, abi.ra8_camera_source_capture(&bound, &buffer, &out));
    try std.testing.expect(out.data == null);
}

test "capture: a well-behaved backend frame survives every contract check" {
    reset();
    fault.frame = validFrame();
    var bound = abi.Source{ .iface = &full_source, .ctx = null };
    var out = core.Frame{};
    const buffer = core.Buffer{ .data = &pixels, .capacity = pixels.len };
    try std.testing.expectEqual(err.ok, abi.ra8_camera_source_capture(&bound, &buffer, &out));
    try std.testing.expect(out.data == @as(?[*]const u8, &pixels));
    try std.testing.expectEqual(@as(u32, 3), out.bytes);
}

test "encode: all four arguments are judged before the vtable" {
    reset();
    var codec = abi.Codec{};
    var frame = validFrame();
    const buffer = core.Buffer{ .data = &pixels, .capacity = pixels.len };
    var out = core.Frame{ .bytes = 5 };
    try std.testing.expectEqual(err.null_ptr, abi.ra8_camera_codec_encode(null, &frame, &buffer, &out));
    try std.testing.expectEqual(err.null_ptr, abi.ra8_camera_codec_encode(&codec, null, &buffer, &out));
    try std.testing.expectEqual(err.null_ptr, abi.ra8_camera_codec_encode(&codec, &frame, null, &out));
    try std.testing.expectEqual(err.null_ptr, abi.ra8_camera_codec_encode(&codec, &frame, &buffer, null));
    // No argument rejection has touched the caller's output yet.
    try std.testing.expectEqual(@as(u32, 5), out.bytes);
}

test "encode: an unbound codec or absent encode row is not_initialized" {
    reset();
    var frame = validFrame();
    const buffer = core.Buffer{ .data = &pixels, .capacity = pixels.len };
    var out = core.Frame{};
    var unbound = abi.Codec{};
    try std.testing.expectEqual(err.not_initialized, abi.ra8_camera_codec_encode(&unbound, &frame, &buffer, &out));
    var half = abi.Codec{ .iface = &no_encode };
    try std.testing.expectEqual(err.not_initialized, abi.ra8_camera_codec_encode(&half, &frame, &buffer, &out));
    try std.testing.expectEqual(@as(u32, 0), fault.calls);
}

test "encode: an invalid input is rejected before dispatch" {
    reset();
    var bad = core.Frame{ .data = null, .bytes = 3, .width = 1, .height = 1, .format = jpeg };
    const buffer = core.Buffer{ .data = &pixels, .capacity = pixels.len };
    var out = core.Frame{};
    var codec = abi.Codec{ .iface = &full_codec, .ctx = null };
    try std.testing.expectEqual(err.null_ptr, abi.ra8_camera_codec_encode(&codec, &bad, &buffer, &out));
    try std.testing.expectEqual(@as(u32, 0), fault.calls);
}

test "encode: a backend error is forwarded and clears the output" {
    reset();
    fault.result = 0x109;
    fault.frame = validFrame();
    var frame = validFrame();
    const buffer = core.Buffer{ .data = &pixels, .capacity = pixels.len };
    var out = core.Frame{};
    var codec = abi.Codec{ .iface = &full_codec, .ctx = null };
    try std.testing.expectEqual(@as(u16, 0x109), abi.ra8_camera_codec_encode(&codec, &frame, &buffer, &out));
    try std.testing.expect(out.data == null);
    try std.testing.expectEqual(@as(u32, 1), fault.calls);
}

test "encode: a successful but invalid output frame is rejected and cleared" {
    reset();
    fault.result = err.ok;
    fault.frame = .{ .data = null, .bytes = 4, .width = 1, .height = 1, .format = jpeg };
    var frame = validFrame();
    const buffer = core.Buffer{ .data = &pixels, .capacity = pixels.len };
    var out = core.Frame{};
    var codec = abi.Codec{ .iface = &full_codec, .ctx = null };
    try std.testing.expectEqual(err.null_ptr, abi.ra8_camera_codec_encode(&codec, &frame, &buffer, &out));
    try std.testing.expect(out.data == null);
    try std.testing.expectEqual(@as(u32, 1), fault.calls);
}

test "encode: a valid output frame is published unchanged" {
    reset();
    fault.result = err.ok;
    fault.frame = .{ .data = &pixels, .bytes = 4, .width = 2, .height = 2, .format = jpeg };
    var frame = validFrame();
    const buffer = core.Buffer{ .data = &pixels, .capacity = pixels.len };
    var out = core.Frame{};
    var codec = abi.Codec{ .iface = &full_codec, .ctx = null };
    try std.testing.expectEqual(err.ok, abi.ra8_camera_codec_encode(&codec, &frame, &buffer, &out));
    try std.testing.expectEqual(@as(u32, 4), out.bytes);
    try std.testing.expectEqual(@as(u8, jpeg), out.format);
}

test "encode: an empty output buffer still reaches the backend" {
    // The facade never inspects the output buffer's contents: a zero-capacity
    // buffer is legal for a documented zero-copy codec.
    reset();
    fault.result = err.ok;
    fault.frame = .{ .data = &pixels, .bytes = 4, .width = 2, .height = 2, .format = jpeg };
    var frame = validFrame();
    const empty = core.Buffer{};
    var out = core.Frame{};
    var codec = abi.Codec{ .iface = &full_codec, .ctx = null };
    try std.testing.expectEqual(err.ok, abi.ra8_camera_codec_encode(&codec, &frame, &empty, &out));
    try std.testing.expectEqual(@as(u32, 1), fault.calls);
}

test "handle validation: the exported guard order is null then iface then rows" {
    var unbound = abi.Source{};
    try std.testing.expectEqual(err.null_ptr, abi.sourceHandleErr(null));
    try std.testing.expectEqual(err.not_initialized, abi.sourceHandleErr(&unbound));
    var without_info = abi.Source{ .iface = &no_get_info };
    try std.testing.expectEqual(err.not_initialized, abi.sourceHandleErr(&without_info));
    var without_capture = abi.Source{ .iface = &no_capture };
    try std.testing.expectEqual(err.not_initialized, abi.sourceHandleErr(&without_capture));
    var complete = abi.Source{ .iface = &full_source };
    try std.testing.expectEqual(err.ok, abi.sourceHandleErr(&complete));
}

test "translation: every fault enum has an ra8_err_t" {
    try std.testing.expectEqual(err.ok, abi.frameErr(.ok));
    try std.testing.expectEqual(err.null_ptr, abi.frameErr(.null_data));
    try std.testing.expectEqual(err.invalid_arg, abi.frameErr(.zero_width));
    try std.testing.expectEqual(err.invalid_arg, abi.frameErr(.odd_uyvy_width));
    try std.testing.expectEqual(err.invalid_size, abi.frameErr(.bytes_short));
    try std.testing.expectEqual(err.ok, abi.bufferErr(.ok));
    try std.testing.expectEqual(err.null_ptr, abi.bufferErr(.null_data));
    try std.testing.expectEqual(err.invalid_size, abi.bufferErr(.zero_capacity));
    try std.testing.expectEqual(err.ok, abi.captureErr(.ok));
    try std.testing.expectEqual(err.invalid_size, abi.captureErr(.alias_mismatch));
    try std.testing.expectEqual(err.invalid_size, abi.captureErr(.bytes_exceed_capacity));
}

test "layouts: the vtables and handles match the private C header" {
    const ptr_bytes = @sizeOf(usize);
    try std.testing.expectEqual(ptr_bytes * 2, @sizeOf(abi.SourceIface));
    try std.testing.expectEqual(ptr_bytes, @sizeOf(abi.CodecIface));
    try std.testing.expectEqual(ptr_bytes * 2, @sizeOf(abi.Source));
    try std.testing.expectEqual(ptr_bytes * 2, @sizeOf(abi.Codec));
    const zeroed = abi.Source{};
    try std.testing.expect(zeroed.iface == null);
    try std.testing.expect(zeroed.ctx == null);
}
