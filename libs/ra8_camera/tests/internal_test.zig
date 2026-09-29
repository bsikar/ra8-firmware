//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Tests for the pure camera policy in `src/internal/root.zig`: the frame
//! geometry rules, the post-dispatch capture contract, the software-JPEG
//! configuration bounds, BT.601 colour conversion, and nearest-neighbour
//! sampling. Nothing here links the ABI membrane or any extern.

const std = @import("std");
const core = @import("implementation");

const rgb888 = core.format.rgb888;
const uyvy422 = core.format.uyvy422;
const jpeg = core.format.jpeg;

var pixels: [16 * 16 * 3]u8 = undefined;

fn rgbFrame() core.Frame {
    return .{
        .data = &pixels,
        .bytes = 16 * 16 * 3,
        .stride_bytes = 16 * 3,
        .width = 16,
        .height = 16,
        .format = rgb888,
    };
}

test "row bytes: rgb888 is three bytes per pixel" {
    try std.testing.expectEqual(@as(u32, 48), core.rowBytesOf(rgb888, 16).bytes);
    try std.testing.expectEqual(@as(u32, 3), core.rowBytesOf(rgb888, 1).bytes);
}

test "row bytes: uyvy422 is two bytes per pixel and needs an even width" {
    try std.testing.expectEqual(@as(u32, 32), core.rowBytesOf(uyvy422, 16).bytes);
    try std.testing.expectEqual(core.RowBytes.odd_uyvy_width, core.rowBytesOf(uyvy422, 15));
    try std.testing.expectEqual(core.RowBytes.odd_uyvy_width, core.rowBytesOf(uyvy422, 1));
}

test "row bytes: jpeg and every unknown byte are unsupported row formats" {
    try std.testing.expectEqual(core.RowBytes.unsupported_format, core.rowBytesOf(jpeg, 16));
    var value: u8 = 3;
    while (value < 255) : (value += 1) {
        try std.testing.expectEqual(core.RowBytes.unsupported_format, core.rowBytesOf(value, 16));
    }
    try std.testing.expectEqual(core.RowBytes.unsupported_format, core.rowBytesOf(255, 16));
}

test "row bytes: widest uyvy and rgb rows do not overflow" {
    try std.testing.expectEqual(@as(u32, 65534 * 2), core.rowBytesOf(uyvy422, 65534).bytes);
    try std.testing.expectEqual(@as(u32, 65535 * 3), core.rowBytesOf(rgb888, 65535).bytes);
}

test "validate: the packed rgb fixture is the accepted baseline" {
    try std.testing.expectEqual(core.FrameFault.ok, core.validateFrame(rgbFrame()));
}

test "validate: absent bytes are the first rejection" {
    var frame = rgbFrame();
    frame.data = null;
    try std.testing.expectEqual(core.FrameFault.null_data, core.validateFrame(frame));
    // Absent bytes are judged before zero geometry.
    frame.width = 0;
    frame.height = 0;
    try std.testing.expectEqual(core.FrameFault.null_data, core.validateFrame(frame));
}

test "validate: zero width is judged before zero height" {
    var frame = rgbFrame();
    frame.width = 0;
    try std.testing.expectEqual(core.FrameFault.zero_width, core.validateFrame(frame));
    frame.height = 0;
    try std.testing.expectEqual(core.FrameFault.zero_width, core.validateFrame(frame));
    frame = rgbFrame();
    frame.height = 0;
    try std.testing.expectEqual(core.FrameFault.zero_height, core.validateFrame(frame));
}

test "validate: a jpeg frame needs bytes and no stride" {
    var frame = core.Frame{
        .data = &pixels,
        .bytes = 2,
        .stride_bytes = 0,
        .width = 1,
        .height = 1,
        .format = jpeg,
    };
    try std.testing.expectEqual(core.FrameFault.ok, core.validateFrame(frame));
    frame.bytes = 0;
    try std.testing.expectEqual(core.FrameFault.jpeg_zero_bytes, core.validateFrame(frame));
    frame.bytes = 2;
    frame.stride_bytes = 1;
    try std.testing.expectEqual(core.FrameFault.jpeg_stride_set, core.validateFrame(frame));
    // A zero byte count is judged before a set stride.
    frame.bytes = 0;
    try std.testing.expectEqual(core.FrameFault.jpeg_zero_bytes, core.validateFrame(frame));
}

test "validate: an unknown format is rejected only after the geometry guards" {
    var frame = rgbFrame();
    frame.format = 99;
    try std.testing.expectEqual(core.FrameFault.unsupported_format, core.validateFrame(frame));
    frame.width = 0;
    try std.testing.expectEqual(core.FrameFault.zero_width, core.validateFrame(frame));
}

test "validate: an odd uyvy width is its own rejection" {
    const frame = core.Frame{
        .data = &pixels,
        .bytes = 16 * 16 * 2,
        .stride_bytes = 16 * 2,
        .width = 15,
        .height = 16,
        .format = uyvy422,
    };
    try std.testing.expectEqual(core.FrameFault.odd_uyvy_width, core.validateFrame(frame));
}

test "validate: a stride shorter than one packed row is rejected" {
    var frame = rgbFrame();
    frame.stride_bytes -= 1;
    try std.testing.expectEqual(core.FrameFault.stride_short, core.validateFrame(frame));
    frame.stride_bytes = 0;
    try std.testing.expectEqual(core.FrameFault.stride_short, core.validateFrame(frame));
}

test "validate: a stride whose row product wraps is rejected before the byte check" {
    const frame = core.Frame{
        .data = &pixels,
        .bytes = std.math.maxInt(u32),
        .stride_bytes = 0x80000000,
        .width = 16,
        .height = 4,
        .format = rgb888,
    };
    try std.testing.expectEqual(core.FrameFault.height_overflows_stride, core.validateFrame(frame));
    // maxInt(u32) / 0x80000000 is 1, so a single row is the whole budget: at
    // the boundary the product still fits and the byte count decides instead.
    const edge = core.Frame{
        .data = &pixels,
        .bytes = std.math.maxInt(u32),
        .stride_bytes = 0x80000000,
        .width = 16,
        .height = 1,
        .format = rgb888,
    };
    try std.testing.expectEqual(core.FrameFault.ok, core.validateFrame(edge));
    var short = edge;
    short.bytes = 0x7FFFFFFF;
    try std.testing.expectEqual(core.FrameFault.bytes_short, core.validateFrame(short));
}

test "validate: the byte count must cover every row" {
    var frame = rgbFrame();
    frame.bytes -= 1;
    try std.testing.expectEqual(core.FrameFault.bytes_short, core.validateFrame(frame));
    frame = rgbFrame();
    frame.bytes += 1;
    try std.testing.expectEqual(core.FrameFault.ok, core.validateFrame(frame));
}

test "validate: a uyvy frame with a generous stride is accepted" {
    const frame = core.Frame{
        .data = &pixels,
        .bytes = 16 * 40,
        .stride_bytes = 40,
        .width = 16,
        .height = 16,
        .format = uyvy422,
    };
    try std.testing.expectEqual(core.FrameFault.ok, core.validateFrame(frame));
}

test "capture buffer: absent bytes are judged before a zero capacity" {
    try std.testing.expectEqual(core.BufferFault.ok, core.captureBufferFault(true, 1));
    try std.testing.expectEqual(core.BufferFault.null_data, core.captureBufferFault(false, 16));
    try std.testing.expectEqual(core.BufferFault.null_data, core.captureBufferFault(false, 0));
    try std.testing.expectEqual(core.BufferFault.zero_capacity, core.captureBufferFault(true, 0));
}

test "capture contract: the frame must alias the buffer and fit inside it" {
    try std.testing.expectEqual(core.CaptureFault.ok, core.capturePostFault(0x1000, 0x1000, 16, 16));
    try std.testing.expectEqual(core.CaptureFault.alias_mismatch, core.capturePostFault(0x1004, 0x1000, 16, 16));
    try std.testing.expectEqual(core.CaptureFault.bytes_exceed_capacity, core.capturePostFault(0x1000, 0x1000, 17, 16));
    // The alias is judged first, so a doubly-faulty capture reports it.
    try std.testing.expectEqual(core.CaptureFault.alias_mismatch, core.capturePostFault(0, 0x1000, 17, 16));
}

test "memory source: the capture buffer must hold the whole fixed frame" {
    try std.testing.expect(core.memoryCapacityFits(36, 36));
    try std.testing.expect(core.memoryCapacityFits(40, 36));
    try std.testing.expect(!core.memoryCapacityFits(35, 36));
}

test "jpeg cfg: a valid configuration is the accepted baseline" {
    try std.testing.expectEqual(core.JpegCfgFault.ok, core.validateJpegCfg(true, 75, 8, 8, 8 * 8 * 3));
}

test "jpeg cfg: the workspace pointer is judged first" {
    try std.testing.expectEqual(core.JpegCfgFault.null_workspace, core.validateJpegCfg(false, 75, 8, 8, 192));
    try std.testing.expectEqual(core.JpegCfgFault.null_workspace, core.validateJpegCfg(false, 0, 0, 0, 0));
}

test "jpeg cfg: both quality bounds are enforced" {
    try std.testing.expectEqual(core.JpegCfgFault.quality_below_min, core.validateJpegCfg(true, 0, 8, 8, 192));
    try std.testing.expectEqual(core.JpegCfgFault.ok, core.validateJpegCfg(true, core.jpeg_quality_min, 8, 8, 192));
    try std.testing.expectEqual(core.JpegCfgFault.ok, core.validateJpegCfg(true, core.jpeg_quality_max, 8, 8, 192));
    try std.testing.expectEqual(core.JpegCfgFault.quality_above_max, core.validateJpegCfg(true, 101, 8, 8, 192));
    try std.testing.expectEqual(core.JpegCfgFault.quality_above_max, core.validateJpegCfg(true, 255, 8, 8, 192));
}

test "jpeg cfg: zero output width is judged before zero height" {
    try std.testing.expectEqual(core.JpegCfgFault.zero_output_width, core.validateJpegCfg(true, 75, 0, 8, 192));
    try std.testing.expectEqual(core.JpegCfgFault.zero_output_width, core.validateJpegCfg(true, 75, 0, 0, 192));
    try std.testing.expectEqual(core.JpegCfgFault.zero_output_height, core.validateJpegCfg(true, 75, 8, 0, 192));
}

test "jpeg cfg: the overflow guard catches geometry the capacity check would accept" {
    // 23307 * 3 * 61426 passes 2^32 by 50 bytes, so a wrapped product would be
    // compared against 50 and a 192-byte workspace would look sufficient.
    try std.testing.expectEqual(
        core.JpegCfgFault.geometry_overflows,
        core.validateJpegCfg(true, 75, 23307, 61426, 192),
    );
    try std.testing.expectEqual(
        core.JpegCfgFault.workspace_short,
        core.validateJpegCfg(true, 75, 23307, 61425, 192),
    );
}

test "jpeg cfg: the workspace must cover the packed output image" {
    try std.testing.expectEqual(core.JpegCfgFault.workspace_short, core.validateJpegCfg(true, 75, 8, 8, 191));
    try std.testing.expectEqual(core.JpegCfgFault.ok, core.validateJpegCfg(true, 75, 8, 8, 191 + 1));
    try std.testing.expectEqual(@as(u32, 192), core.jpegWorkspaceBytes(8, 8));
}

test "clamp: saturates below zero and above 255 and passes the range through" {
    try std.testing.expectEqual(@as(u8, 0), core.clampByte(-1));
    try std.testing.expectEqual(@as(u8, 0), core.clampByte(std.math.minInt(i32)));
    try std.testing.expectEqual(@as(u8, 0), core.clampByte(0));
    try std.testing.expectEqual(@as(u8, 255), core.clampByte(255));
    try std.testing.expectEqual(@as(u8, 255), core.clampByte(256));
    try std.testing.expectEqual(@as(u8, 255), core.clampByte(std.math.maxInt(i32)));
    var value: i32 = 0;
    while (value <= 255) : (value += 1) {
        try std.testing.expectEqual(@as(u8, @intCast(value)), core.clampByte(value));
    }
}

test "ycbcr: neutral chroma reproduces the limited-range luma ramp" {
    try std.testing.expectEqual([3]u8{ 0, 0, 0 }, core.ycbcrToRgb(16, 128, 128));
    try std.testing.expectEqual([3]u8{ 255, 255, 255 }, core.ycbcrToRgb(235, 128, 128));
    const mid = core.ycbcrToRgb(126, 128, 128);
    try std.testing.expectEqual(mid[0], mid[1]);
    try std.testing.expectEqual(mid[1], mid[2]);
}

test "ycbcr: the clamp fixture saturates in all three directions at once" {
    // Luma below the limited-range floor with both chroma samples at maximum:
    // the luma floor, the low green clamp, and the high blue clamp all fire.
    try std.testing.expectEqual([3]u8{ 203, 0, 255 }, core.ycbcrToRgb(0, 255, 255));
    // The floor means every sub-black luma produces the same pixel.
    try std.testing.expectEqual(core.ycbcrToRgb(0, 255, 255), core.ycbcrToRgb(16, 255, 255));
}

test "ycbcr: minimum chroma drives green high and blue to the floor" {
    // Full luma with both chroma samples at zero: green saturates high, blue
    // clamps at the floor, and red keeps its unsaturated fixed-point value.
    try std.testing.expectEqual([3]u8{ 50, 255, 0 }, core.ycbcrToRgb(235, 0, 0));
}

test "offsets: rgb, uyvy pair, and luma selection" {
    try std.testing.expectEqual(@as(u32, 0), core.rgbOffset(48, 0, 0));
    try std.testing.expectEqual(@as(u32, 51), core.rgbOffset(48, 1, 1));
    try std.testing.expectEqual(@as(u32, 0), core.uyvyPairOffset(32, 0, 0));
    try std.testing.expectEqual(@as(u32, 0), core.uyvyPairOffset(32, 1, 0));
    try std.testing.expectEqual(@as(u32, 4), core.uyvyPairOffset(32, 2, 0));
    try std.testing.expectEqual(@as(u32, 36), core.uyvyPairOffset(32, 2, 1));
    try std.testing.expectEqual(@as(u32, 1), core.uyvyLumaOffset(0));
    try std.testing.expectEqual(@as(u32, 3), core.uyvyLumaOffset(1));
    try std.testing.expectEqual(@as(u32, 1), core.uyvyLumaOffset(2));
    try std.testing.expectEqual(@as(u32, 3), core.uyvyLumaOffset(3));
}

test "offsets: address arithmetic wraps instead of trapping" {
    try std.testing.expectEqual(@as(u32, 0), core.rgbOffset(0x80000000, 0, 2));
    try std.testing.expectEqual(@as(u32, 6), core.rgbOffset(0x80000000, 2, 2));
    try std.testing.expectEqual(@as(u32, 0), core.uyvyPairOffset(0x80000000, 0, 2));
}

test "sampling: nearest-neighbour source coordinates and workspace offsets" {
    try std.testing.expectEqual(@as(u32, 0), core.sourceCoord(0, 16, 8));
    try std.testing.expectEqual(@as(u32, 2), core.sourceCoord(1, 16, 8));
    try std.testing.expectEqual(@as(u32, 14), core.sourceCoord(7, 16, 8));
    // A non-multiple input width is what makes a sampled column odd, the only
    // route to the second luma of a UYVY chroma pair.
    try std.testing.expectEqual(@as(u32, 1), core.sourceCoord(1, 12, 8));
    try std.testing.expectEqual(@as(u32, 3), core.sourceCoord(2, 12, 8));
    try std.testing.expectEqual(@as(usize, 0), core.workspaceOffset(0, 0, 8));
    try std.testing.expectEqual(@as(usize, 3), core.workspaceOffset(1, 0, 8));
    try std.testing.expectEqual(@as(usize, 24), core.workspaceOffset(0, 1, 8));
    try std.testing.expectEqual(@as(usize, 189), core.workspaceOffset(7, 7, 8));
}

test "read: rgb888 returns the packed triple at the row offset" {
    var bytes = [_]u8{ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
    try std.testing.expectEqual([3]u8{ 1, 2, 3 }, core.readRgb(&bytes, rgb888, 6, 0, 0));
    try std.testing.expectEqual([3]u8{ 4, 5, 6 }, core.readRgb(&bytes, rgb888, 6, 1, 0));
    try std.testing.expectEqual([3]u8{ 7, 8, 9 }, core.readRgb(&bytes, rgb888, 6, 0, 1));
}

test "read: uyvy selects y0 for even columns and y1 for odd ones" {
    // Cb Y0 Cr Y1 with neutral chroma, so the result is the luma ramp.
    var bytes = [_]u8{ 128, 16, 128, 235 };
    try std.testing.expectEqual([3]u8{ 0, 0, 0 }, core.readRgb(&bytes, uyvy422, 4, 0, 0));
    try std.testing.expectEqual([3]u8{ 255, 255, 255 }, core.readRgb(&bytes, uyvy422, 4, 1, 0));
}

test "prepare: a flat rgb input fills the whole workspace with one pixel" {
    var source: [4 * 4 * 3]u8 = undefined;
    var index: usize = 0;
    while (index < source.len) : (index += 3) {
        source[index] = 10;
        source[index + 1] = 20;
        source[index + 2] = 30;
    }
    var workspace = [_]u8{0} ** (2 * 2 * 3);
    core.prepareRgb(&source, rgb888, 4 * 3, 4, 4, &workspace, 2, 2);
    var offset: usize = 0;
    while (offset < workspace.len) : (offset += 3) {
        try std.testing.expectEqual(@as(u8, 10), workspace[offset]);
        try std.testing.expectEqual(@as(u8, 20), workspace[offset + 1]);
        try std.testing.expectEqual(@as(u8, 30), workspace[offset + 2]);
    }
}

test "prepare: downscaling picks the nearest source pixel per output pixel" {
    // Four distinct 2x2 quadrants in a 4x4 RGB source; a 2x2 output must land
    // exactly one pixel from each.
    var source: [4 * 4 * 3]u8 = undefined;
    var y: usize = 0;
    while (y < 4) : (y += 1) {
        var x: usize = 0;
        while (x < 4) : (x += 1) {
            const quadrant: u8 = @intCast(((y / 2) * 2) + (x / 2));
            const at = (y * 12) + (x * 3);
            source[at] = quadrant;
            source[at + 1] = quadrant + 100;
            source[at + 2] = quadrant + 200;
        }
    }
    var workspace = [_]u8{0} ** (2 * 2 * 3);
    core.prepareRgb(&source, rgb888, 12, 4, 4, &workspace, 2, 2);
    var slot: u8 = 0;
    while (slot < 4) : (slot += 1) {
        const at = @as(usize, slot) * 3;
        try std.testing.expectEqual(slot, workspace[at]);
        try std.testing.expectEqual(slot + 100, workspace[at + 1]);
        try std.testing.expectEqual(slot + 200, workspace[at + 2]);
    }
}

test "prepare: upscaling repeats source pixels and writes every output byte" {
    var source = [_]u8{ 1, 2, 3, 250, 251, 252 };
    var workspace = [_]u8{0xAA} ** (4 * 1 * 3);
    core.prepareRgb(&source, rgb888, 6, 2, 1, &workspace, 4, 1);
    try std.testing.expectEqual([3]u8{ 1, 2, 3 }, workspace[0..3].*);
    try std.testing.expectEqual([3]u8{ 1, 2, 3 }, workspace[3..6].*);
    try std.testing.expectEqual([3]u8{ 250, 251, 252 }, workspace[6..9].*);
    try std.testing.expectEqual([3]u8{ 250, 251, 252 }, workspace[9..12].*);
}

test "prepare: a uyvy source sampled at an odd width saturates every pixel" {
    // The C suite's colour-clamp vector: every pair is Cb/Cr 255 with sub-black
    // luma, and width 12 into an 8-wide output makes some sampled columns odd.
    var source = [_]u8{0} ** (12 * 16 * 2);
    var index: usize = 0;
    while (index < source.len) : (index += 2) {
        source[index] = 255;
        source[index + 1] = 0;
    }
    var workspace = [_]u8{0} ** (8 * 8 * 3);
    core.prepareRgb(&source, uyvy422, 12 * 2, 12, 16, &workspace, 8, 8);
    var offset: usize = 0;
    while (offset < workspace.len) : (offset += 3) {
        try std.testing.expectEqual(@as(u8, 203), workspace[offset]);
        try std.testing.expectEqual(@as(u8, 0), workspace[offset + 1]);
        try std.testing.expectEqual(@as(u8, 255), workspace[offset + 2]);
    }
}

test "layouts: the C ABI structs keep their documented shape" {
    const ptr_bytes = @sizeOf(usize);
    try std.testing.expectEqual(ptr_bytes * 2, @sizeOf(core.Buffer));
    try std.testing.expectEqual(@as(usize, 16), @sizeOf(core.Info));
    try std.testing.expectEqual(ptr_bytes, @offsetOf(core.Frame, "bytes"));
    try std.testing.expectEqual(ptr_bytes + 12, @offsetOf(core.Frame, "format"));
    // A zeroed descriptor is the documented initial value on both sides.
    const zeroed = core.Frame{};
    try std.testing.expect(zeroed.data == null);
    try std.testing.expectEqual(@as(u8, core.format.rgb888), zeroed.format);
}
