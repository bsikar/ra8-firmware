//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Tests for the three ported backends driven through their real vtable rows,
//! the same route `tests/misc/src/test_ra8_camera_source_memory.c` and
//! `test_ra8_camera.c` take to reach the rows the facade shields behind its own
//! argument guards. `ra8_jpeg_sw_encode` is exported here as a recording fake,
//! exactly as the C build substitutes it at link time.

const std = @import("std");
const camera = @import("camera");

const abi = camera.abi;
const core = abi.core;
const err = abi.err;
const memory = camera.memory;
const passthrough = camera.passthrough;
const jpeg_sw = camera.jpeg_sw;
const rgb888 = core.format.rgb888;
const uyvy422 = core.format.uyvy422;
const jpeg = core.format.jpeg;

/// What the fake encoder saw and what it will answer.
const Encoder = struct {
    calls: u32 = 0,
    width: u16 = 0,
    height: u16 = 0,
    quality: u8 = 0,
    capacity: u32 = 0,
    first_workspace_byte: u8 = 0,
    produced: u32 = 4,
    result: u16 = err.ok,
};

var encoder: Encoder = .{};

export fn ra8_jpeg_sw_encode(
    rgb_buf: [*]const u8,
    width: u16,
    height: u16,
    quality: u8,
    out_buf: [*]u8,
    out_capacity: u32,
    out_bytes: *u32,
) callconv(.c) u16 {
    encoder.calls += 1;
    encoder.width = width;
    encoder.height = height;
    encoder.quality = quality;
    encoder.capacity = out_capacity;
    encoder.first_workspace_byte = rgb_buf[0];
    if (encoder.result != err.ok) {
        return encoder.result;
    }
    var index: u32 = 0;
    while (index < encoder.produced) : (index += 1) {
        out_buf[index] = @truncate(0xD8 + index);
    }
    out_bytes.* = encoder.produced;
    return err.ok;
}

var fixture: [4 * 3 * 3]u8 = undefined;
var capture_bytes: [40]u8 = undefined;
var workspace: [8 * 8 * 3]u8 = undefined;
var encoded_bytes: [64]u8 = undefined;

fn fillFixture() void {
    var index: usize = 0;
    while (index < fixture.len) : (index += 1) {
        fixture[index] = @truncate((index * 37) + 11);
    }
}

/// A 4x3 packed RGB888 frame over the shared fixture, 36 bytes in all.
fn fixedFrame() core.Frame {
    return .{
        .data = &fixture,
        .bytes = 36,
        .stride_bytes = 12,
        .width = 4,
        .height = 3,
        .format = rgb888,
    };
}

test "memory init: every argument is judged in order" {
    var source = abi.Source{};
    var state = memory.State{};
    var frame = fixedFrame();
    try std.testing.expectEqual(err.null_ptr, memory.ra8_camera_source_memory_init(null, &state, &frame));
    try std.testing.expectEqual(err.null_ptr, memory.ra8_camera_source_memory_init(&source, null, &frame));
    try std.testing.expectEqual(err.null_ptr, memory.ra8_camera_source_memory_init(&source, &state, null));
    try std.testing.expect(source.iface == null);
}

test "memory init: the fixed frame must validate before it is bound" {
    var source = abi.Source{};
    var state = memory.State{};
    var frame = fixedFrame();
    frame.stride_bytes = 8;
    try std.testing.expectEqual(err.invalid_size, memory.ra8_camera_source_memory_init(&source, &state, &frame));
    frame = fixedFrame();
    frame.width = 0;
    try std.testing.expectEqual(err.invalid_arg, memory.ra8_camera_source_memory_init(&source, &state, &frame));
    frame = fixedFrame();
    frame.data = null;
    try std.testing.expectEqual(err.null_ptr, memory.ra8_camera_source_memory_init(&source, &state, &frame));
    // Nothing was bound by any rejection.
    try std.testing.expect(source.iface == null);
    try std.testing.expect(state.frame.data == null);
}

test "memory init: a valid frame binds both vtable rows" {
    var source = abi.Source{};
    var state = memory.State{};
    var frame = fixedFrame();
    try std.testing.expectEqual(err.ok, memory.ra8_camera_source_memory_init(&source, &state, &frame));
    try std.testing.expectEqual(err.ok, abi.sourceHandleErr(&source));
    try std.testing.expectEqual(@as(u32, 36), state.frame.bytes);
    try std.testing.expect(source.ctx == @as(?*anyopaque, &state));
}

test "memory get_info: reports the fixed frame's geometry" {
    var source = abi.Source{};
    var state = memory.State{};
    var frame = fixedFrame();
    try std.testing.expectEqual(err.ok, memory.ra8_camera_source_memory_init(&source, &state, &frame));
    var info = core.Info{};
    try std.testing.expectEqual(err.ok, abi.ra8_camera_source_get_info(&source, &info));
    try std.testing.expectEqual(@as(u32, 36), info.frame_bytes_max);
    try std.testing.expectEqual(@as(u32, 12), info.stride_bytes);
    try std.testing.expectEqual(@as(u16, 4), info.width);
    try std.testing.expectEqual(@as(u16, 3), info.height);
    try std.testing.expectEqual(@as(u8, rgb888), info.format);
}

test "memory get_info: the row's own null guards are reachable through the vtable" {
    var source = abi.Source{};
    var state = memory.State{};
    var frame = fixedFrame();
    try std.testing.expectEqual(err.ok, memory.ra8_camera_source_memory_init(&source, &state, &frame));
    var info = core.Info{};
    const row = source.iface.?.get_info.?;
    try std.testing.expectEqual(err.null_ptr, row(null, &info));
    try std.testing.expectEqual(err.null_ptr, row(source.ctx, null));
}

test "memory capture: replays the fixed frame byte for byte" {
    fillFixture();
    @memset(&capture_bytes, 0xA5);
    var source = abi.Source{};
    var state = memory.State{};
    var frame = fixedFrame();
    try std.testing.expectEqual(err.ok, memory.ra8_camera_source_memory_init(&source, &state, &frame));
    const buffer = core.Buffer{ .data = &capture_bytes, .capacity = capture_bytes.len };
    var out = core.Frame{};
    try std.testing.expectEqual(err.ok, abi.ra8_camera_source_capture(&source, &buffer, &out));
    try std.testing.expect(out.data == @as(?[*]const u8, &capture_bytes));
    try std.testing.expectEqual(@as(u32, 36), out.bytes);
    try std.testing.expectEqualSlices(u8, fixture[0..36], capture_bytes[0..36]);
    // The oversized tail is untouched, so the copy is bounded by the frame.
    try std.testing.expectEqual(@as(u8, 0xA5), capture_bytes[36]);
    try std.testing.expectEqual(@as(u8, 0xA5), capture_bytes[39]);
}

test "memory capture: an undersized buffer is invalid_size and clears the output" {
    fillFixture();
    var source = abi.Source{};
    var state = memory.State{};
    var frame = fixedFrame();
    try std.testing.expectEqual(err.ok, memory.ra8_camera_source_memory_init(&source, &state, &frame));
    const tiny = core.Buffer{ .data = &capture_bytes, .capacity = 16 };
    var out = core.Frame{};
    try std.testing.expectEqual(err.invalid_size, abi.ra8_camera_source_capture(&source, &tiny, &out));
    try std.testing.expect(out.data == null);
    // Exactly enough capacity is accepted.
    const exact = core.Buffer{ .data = &capture_bytes, .capacity = 36 };
    try std.testing.expectEqual(err.ok, abi.ra8_camera_source_capture(&source, &exact, &out));
}

test "memory capture: the row's own null guards are reachable through the vtable" {
    var source = abi.Source{};
    var state = memory.State{};
    var frame = fixedFrame();
    try std.testing.expectEqual(err.ok, memory.ra8_camera_source_memory_init(&source, &state, &frame));
    const buffer = core.Buffer{ .data = &capture_bytes, .capacity = capture_bytes.len };
    var out = core.Frame{};
    const row = source.iface.?.capture.?;
    try std.testing.expectEqual(err.null_ptr, row(null, &buffer, &out));
    try std.testing.expectEqual(err.null_ptr, row(source.ctx, null, &out));
    try std.testing.expectEqual(err.null_ptr, row(source.ctx, &buffer, null));
}

test "passthrough init: a null handle is null_ptr and binding is stateless" {
    try std.testing.expectEqual(err.null_ptr, passthrough.ra8_camera_codec_passthrough_init(null));
    var codec = abi.Codec{ .ctx = @ptrCast(&fixture) };
    try std.testing.expectEqual(err.ok, passthrough.ra8_camera_codec_passthrough_init(&codec));
    try std.testing.expect(codec.iface != null);
    // Binding clears any stale context: the backend has no state.
    try std.testing.expect(codec.ctx == null);
}

test "passthrough: a jpeg frame is returned aliasing its input" {
    var stream = [_]u8{ 0xFF, 0xD8, 0xFF, 0xD9 };
    var input = core.Frame{ .data = &stream, .bytes = 4, .width = 2, .height = 3, .format = jpeg };
    const unused = core.Buffer{};
    var codec = abi.Codec{};
    try std.testing.expectEqual(err.ok, passthrough.ra8_camera_codec_passthrough_init(&codec));
    var out = core.Frame{};
    try std.testing.expectEqual(err.ok, abi.ra8_camera_codec_encode(&codec, &input, &unused, &out));
    try std.testing.expect(out.data == @as(?[*]const u8, &stream));
    try std.testing.expectEqual(@as(u32, 4), out.bytes);
    try std.testing.expectEqual(@as(u16, 2), out.width);
    try std.testing.expectEqual(@as(u16, 3), out.height);
}

test "passthrough: raw input is not_supported and clears the output" {
    fillFixture();
    var input = fixedFrame();
    const unused = core.Buffer{};
    var codec = abi.Codec{};
    try std.testing.expectEqual(err.ok, passthrough.ra8_camera_codec_passthrough_init(&codec));
    var out = core.Frame{};
    try std.testing.expectEqual(err.not_supported, abi.ra8_camera_codec_encode(&codec, &input, &unused, &out));
    try std.testing.expect(out.data == null);
    input.format = uyvy422;
    input.stride_bytes = 8;
    input.bytes = 24;
    try std.testing.expectEqual(err.not_supported, abi.ra8_camera_codec_encode(&codec, &input, &unused, &out));
}

test "passthrough: the row's own null guards are reachable through the vtable" {
    var stream = [_]u8{ 0xFF, 0xD8, 0xFF, 0xD9 };
    var input = core.Frame{ .data = &stream, .bytes = 4, .width = 2, .height = 3, .format = jpeg };
    const unused = core.Buffer{};
    var codec = abi.Codec{};
    try std.testing.expectEqual(err.ok, passthrough.ra8_camera_codec_passthrough_init(&codec));
    var out = core.Frame{};
    const row = codec.iface.?.encode.?;
    try std.testing.expectEqual(err.null_ptr, row(codec.ctx, null, &unused, &out));
    try std.testing.expectEqual(err.null_ptr, row(codec.ctx, &input, &unused, null));
    // A null context is fine: the backend is stateless.
    try std.testing.expectEqual(err.ok, row(null, &input, &unused, &out));
    try std.testing.expect(out.data == @as(?[*]const u8, &stream));
}

fn jpegCfg(capacity: u32) jpeg_sw.Cfg {
    return .{
        .rgb_workspace = &workspace,
        .rgb_workspace_capacity = capacity,
        .output_width = 8,
        .output_height = 8,
        .quality = 75,
    };
}

test "jpeg init: every null argument and every configuration bound" {
    var codec = abi.Codec{};
    var state = jpeg_sw.State{};
    var cfg = jpegCfg(workspace.len);
    try std.testing.expectEqual(err.null_ptr, jpeg_sw.ra8_camera_codec_jpeg_sw_init(null, &state, &cfg));
    try std.testing.expectEqual(err.null_ptr, jpeg_sw.ra8_camera_codec_jpeg_sw_init(&codec, null, &cfg));
    try std.testing.expectEqual(err.null_ptr, jpeg_sw.ra8_camera_codec_jpeg_sw_init(&codec, &state, null));
    cfg.rgb_workspace = null;
    try std.testing.expectEqual(err.null_ptr, jpeg_sw.ra8_camera_codec_jpeg_sw_init(&codec, &state, &cfg));
    cfg = jpegCfg(workspace.len);
    cfg.quality = 0;
    try std.testing.expectEqual(err.invalid_arg, jpeg_sw.ra8_camera_codec_jpeg_sw_init(&codec, &state, &cfg));
    cfg.quality = 101;
    try std.testing.expectEqual(err.invalid_arg, jpeg_sw.ra8_camera_codec_jpeg_sw_init(&codec, &state, &cfg));
    cfg = jpegCfg(workspace.len);
    cfg.output_width = 0;
    try std.testing.expectEqual(err.invalid_arg, jpeg_sw.ra8_camera_codec_jpeg_sw_init(&codec, &state, &cfg));
    cfg = jpegCfg(workspace.len);
    cfg.output_height = 0;
    try std.testing.expectEqual(err.invalid_arg, jpeg_sw.ra8_camera_codec_jpeg_sw_init(&codec, &state, &cfg));
    cfg = jpegCfg(workspace.len - 1);
    try std.testing.expectEqual(err.invalid_size, jpeg_sw.ra8_camera_codec_jpeg_sw_init(&codec, &state, &cfg));
    cfg = jpegCfg(workspace.len);
    cfg.output_width = 23307;
    cfg.output_height = 61426;
    try std.testing.expectEqual(err.invalid_size, jpeg_sw.ra8_camera_codec_jpeg_sw_init(&codec, &state, &cfg));
    try std.testing.expect(codec.iface == null);
}

test "jpeg init: a valid configuration is copied into caller-owned state" {
    var codec = abi.Codec{};
    var state = jpeg_sw.State{};
    var cfg = jpegCfg(workspace.len);
    try std.testing.expectEqual(err.ok, jpeg_sw.ra8_camera_codec_jpeg_sw_init(&codec, &state, &cfg));
    try std.testing.expectEqual(@as(u16, 8), state.cfg.output_width);
    try std.testing.expectEqual(@as(u8, 75), state.cfg.quality);
    try std.testing.expect(codec.ctx == @as(?*anyopaque, &state));
}

test "jpeg encode: an rgb frame is sampled, encoded, and published as jpeg" {
    fillFixture();
    encoder = .{ .produced = 6 };
    @memset(&workspace, 0);
    var codec = abi.Codec{};
    var state = jpeg_sw.State{};
    var cfg = jpegCfg(workspace.len);
    try std.testing.expectEqual(err.ok, jpeg_sw.ra8_camera_codec_jpeg_sw_init(&codec, &state, &cfg));
    var input = fixedFrame();
    const output = core.Buffer{ .data = &encoded_bytes, .capacity = encoded_bytes.len };
    var out = core.Frame{};
    try std.testing.expectEqual(err.ok, abi.ra8_camera_codec_encode(&codec, &input, &output, &out));
    try std.testing.expectEqual(@as(u32, 1), encoder.calls);
    try std.testing.expectEqual(@as(u16, 8), encoder.width);
    try std.testing.expectEqual(@as(u16, 8), encoder.height);
    try std.testing.expectEqual(@as(u8, 75), encoder.quality);
    try std.testing.expectEqual(@as(u32, encoded_bytes.len), encoder.capacity);
    // The encoder saw the sampled workspace, not the zeroed fixture.
    try std.testing.expectEqual(fixture[0], encoder.first_workspace_byte);
    try std.testing.expect(out.data == @as(?[*]const u8, &encoded_bytes));
    try std.testing.expectEqual(@as(u32, 6), out.bytes);
    try std.testing.expectEqual(@as(u32, 0), out.stride_bytes);
    try std.testing.expectEqual(@as(u16, 8), out.width);
    try std.testing.expectEqual(@as(u16, 8), out.height);
    try std.testing.expectEqual(@as(u8, jpeg), out.format);
}

test "jpeg encode: a uyvy frame is converted through the colour transform" {
    encoder = .{ .produced = 4 };
    @memset(&workspace, 0);
    var source = [_]u8{0} ** (12 * 16 * 2);
    var index: usize = 0;
    while (index < source.len) : (index += 2) {
        source[index] = 255;
        source[index + 1] = 0;
    }
    var codec = abi.Codec{};
    var state = jpeg_sw.State{};
    var cfg = jpegCfg(workspace.len);
    try std.testing.expectEqual(err.ok, jpeg_sw.ra8_camera_codec_jpeg_sw_init(&codec, &state, &cfg));
    var input = core.Frame{
        .data = &source,
        .bytes = source.len,
        .stride_bytes = 24,
        .width = 12,
        .height = 16,
        .format = uyvy422,
    };
    const output = core.Buffer{ .data = &encoded_bytes, .capacity = encoded_bytes.len };
    var out = core.Frame{};
    try std.testing.expectEqual(err.ok, abi.ra8_camera_codec_encode(&codec, &input, &output, &out));
    // Every workspace pixel is the triple-saturated BT.601 result.
    var offset: usize = 0;
    while (offset < workspace.len) : (offset += 3) {
        try std.testing.expectEqual(@as(u8, 203), workspace[offset]);
        try std.testing.expectEqual(@as(u8, 0), workspace[offset + 1]);
        try std.testing.expectEqual(@as(u8, 255), workspace[offset + 2]);
    }
}

test "jpeg encode: a compressed input is not_supported and never samples" {
    encoder = .{};
    @memset(&workspace, 0);
    var codec = abi.Codec{};
    var state = jpeg_sw.State{};
    var cfg = jpegCfg(workspace.len);
    try std.testing.expectEqual(err.ok, jpeg_sw.ra8_camera_codec_jpeg_sw_init(&codec, &state, &cfg));
    var input = core.Frame{ .data = &fixture, .bytes = 4, .width = 1, .height = 1, .format = jpeg };
    const output = core.Buffer{ .data = &encoded_bytes, .capacity = encoded_bytes.len };
    var out = core.Frame{};
    try std.testing.expectEqual(err.not_supported, abi.ra8_camera_codec_encode(&codec, &input, &output, &out));
    try std.testing.expectEqual(@as(u32, 0), encoder.calls);
    try std.testing.expectEqual(@as(u8, 0), workspace[1]);
}

test "jpeg encode: both output-buffer guards answer null_ptr before sampling" {
    fillFixture();
    encoder = .{};
    @memset(&workspace, 0);
    var codec = abi.Codec{};
    var state = jpeg_sw.State{};
    var cfg = jpegCfg(workspace.len);
    try std.testing.expectEqual(err.ok, jpeg_sw.ra8_camera_codec_jpeg_sw_init(&codec, &state, &cfg));
    var input = fixedFrame();
    var out = core.Frame{};
    const no_data = core.Buffer{ .data = null, .capacity = encoded_bytes.len };
    try std.testing.expectEqual(err.null_ptr, abi.ra8_camera_codec_encode(&codec, &input, &no_data, &out));
    const empty = core.Buffer{ .data = &encoded_bytes, .capacity = 0 };
    try std.testing.expectEqual(err.null_ptr, abi.ra8_camera_codec_encode(&codec, &input, &empty, &out));
    try std.testing.expect(out.data == null);
    // Both rejections precede conversion, so the workspace is still zeroed.
    try std.testing.expectEqual(@as(u8, 0), workspace[1]);
    try std.testing.expectEqual(@as(u32, 0), encoder.calls);
}

test "jpeg encode: the row's own null guards are reachable through the vtable" {
    fillFixture();
    encoder = .{};
    var codec = abi.Codec{};
    var state = jpeg_sw.State{};
    var cfg = jpegCfg(workspace.len);
    try std.testing.expectEqual(err.ok, jpeg_sw.ra8_camera_codec_jpeg_sw_init(&codec, &state, &cfg));
    var input = fixedFrame();
    const output = core.Buffer{ .data = &encoded_bytes, .capacity = encoded_bytes.len };
    var out = core.Frame{};
    const row = codec.iface.?.encode.?;
    try std.testing.expectEqual(err.null_ptr, row(null, &input, &output, &out));
    try std.testing.expectEqual(err.null_ptr, row(codec.ctx, null, &output, &out));
    try std.testing.expectEqual(err.null_ptr, row(codec.ctx, &input, null, &out));
    try std.testing.expectEqual(err.null_ptr, row(codec.ctx, &input, &output, null));
    try std.testing.expectEqual(@as(u32, 0), encoder.calls);
}

test "jpeg encode: an encoder error is forwarded and publishes nothing" {
    fillFixture();
    encoder = .{ .result = 0x102 };
    var codec = abi.Codec{};
    var state = jpeg_sw.State{};
    var cfg = jpegCfg(workspace.len);
    try std.testing.expectEqual(err.ok, jpeg_sw.ra8_camera_codec_jpeg_sw_init(&codec, &state, &cfg));
    var input = fixedFrame();
    const output = core.Buffer{ .data = &encoded_bytes, .capacity = encoded_bytes.len };
    var out = core.Frame{};
    try std.testing.expectEqual(@as(u16, 0x102), abi.ra8_camera_codec_encode(&codec, &input, &output, &out));
    try std.testing.expect(out.data == null);
    try std.testing.expectEqual(@as(u32, 1), encoder.calls);
}

test "jpeg encode: a zero-byte encode result fails the facade's output check" {
    fillFixture();
    encoder = .{ .produced = 0 };
    var codec = abi.Codec{};
    var state = jpeg_sw.State{};
    var cfg = jpegCfg(workspace.len);
    try std.testing.expectEqual(err.ok, jpeg_sw.ra8_camera_codec_jpeg_sw_init(&codec, &state, &cfg));
    var input = fixedFrame();
    const output = core.Buffer{ .data = &encoded_bytes, .capacity = encoded_bytes.len };
    var out = core.Frame{};
    // The backend reports success with zero bytes; a JPEG view with no bytes is
    // not a valid frame, so the facade rejects it and clears the output.
    try std.testing.expectEqual(err.invalid_size, abi.ra8_camera_codec_encode(&codec, &input, &output, &out));
    try std.testing.expect(out.data == null);
}

test "jpeg cfg translation: every fault has its ra8_err_t" {
    try std.testing.expectEqual(err.ok, jpeg_sw.cfgErr(.ok));
    try std.testing.expectEqual(err.null_ptr, jpeg_sw.cfgErr(.null_workspace));
    try std.testing.expectEqual(err.invalid_arg, jpeg_sw.cfgErr(.quality_below_min));
    try std.testing.expectEqual(err.invalid_arg, jpeg_sw.cfgErr(.quality_above_max));
    try std.testing.expectEqual(err.invalid_arg, jpeg_sw.cfgErr(.zero_output_width));
    try std.testing.expectEqual(err.invalid_arg, jpeg_sw.cfgErr(.zero_output_height));
    try std.testing.expectEqual(err.invalid_size, jpeg_sw.cfgErr(.geometry_overflows));
    try std.testing.expectEqual(err.invalid_size, jpeg_sw.cfgErr(.workspace_short));
}

test "layouts: the backend state structs match the public C headers" {
    const ptr_bytes = @sizeOf(usize);
    try std.testing.expectEqual(@sizeOf(core.Frame), @sizeOf(memory.State));
    try std.testing.expectEqual(ptr_bytes + 4, @offsetOf(jpeg_sw.Cfg, "output_width"));
    try std.testing.expectEqual(ptr_bytes + 8, @offsetOf(jpeg_sw.Cfg, "quality"));
    try std.testing.expectEqual(@sizeOf(jpeg_sw.Cfg), @sizeOf(jpeg_sw.State));
}
