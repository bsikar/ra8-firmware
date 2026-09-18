//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Pure camera policy, with no externs and no ABI surface: the frame geometry
//! rules the facade enforces, the post-dispatch capture contract, the
//! fixed-frame replay bound, the software-JPEG configuration bounds, BT.601
//! limited-range colour conversion, and nearest-neighbour sampling into a
//! caller-owned packed-RGB workspace.

const std = @import("std");

/// `ra8_camera_format_t` values. Kept as raw bytes rather than a Zig enum: the
/// format arrives inside a caller-built descriptor, so an out-of-range byte
/// must stay rejectable instead of being undefined (the same rule the
/// `ra8_batt`, `ra8_mpu` and `ra8_lsm6dso` ports follow for enum-by-value).
pub const format = struct {
    pub const rgb888: u8 = 0;
    pub const uyvy422: u8 = 1;
    pub const jpeg: u8 = 2;
};

/// `ra8_camera_buffer_t`: caller-owned writable byte span.
pub const Buffer = extern struct {
    data: ?[*]u8 = null,
    capacity: u32 = 0,
};

/// `ra8_camera_frame_t`: immutable view of one captured or encoded image.
pub const Frame = extern struct {
    data: ?[*]const u8 = null,
    bytes: u32 = 0,
    stride_bytes: u32 = 0,
    width: u16 = 0,
    height: u16 = 0,
    format: u8 = 0,
};

/// `ra8_camera_source_info_t`: native geometry plus the worst-case capture bound.
pub const Info = extern struct {
    frame_bytes_max: u32 = 0,
    stride_bytes: u32 = 0,
    width: u16 = 0,
    height: u16 = 0,
    format: u8 = 0,
};

const ptr_bytes = @sizeOf(usize);

comptime {
    // The C ABI layouts, asserted on every target the archive is built for.
    std.debug.assert(@offsetOf(Frame, "data") == 0);
    std.debug.assert(@offsetOf(Frame, "bytes") == ptr_bytes);
    std.debug.assert(@offsetOf(Frame, "stride_bytes") == ptr_bytes + 4);
    std.debug.assert(@offsetOf(Frame, "width") == ptr_bytes + 8);
    std.debug.assert(@offsetOf(Frame, "height") == ptr_bytes + 10);
    std.debug.assert(@offsetOf(Frame, "format") == ptr_bytes + 12);
    std.debug.assert(@sizeOf(Frame) == std.mem.alignForward(usize, ptr_bytes + 13, ptr_bytes));

    std.debug.assert(@offsetOf(Buffer, "data") == 0);
    std.debug.assert(@offsetOf(Buffer, "capacity") == ptr_bytes);
    std.debug.assert(@sizeOf(Buffer) == ptr_bytes * 2);

    std.debug.assert(@offsetOf(Info, "frame_bytes_max") == 0);
    std.debug.assert(@offsetOf(Info, "stride_bytes") == 4);
    std.debug.assert(@offsetOf(Info, "width") == 8);
    std.debug.assert(@offsetOf(Info, "height") == 10);
    std.debug.assert(@offsetOf(Info, "format") == 12);
    std.debug.assert(@sizeOf(Info) == 16);
}

/// Outcome of the packed-row-size lookup. The two rejections share one
/// `ra8_err_t` in the C, but stay separate here so each is independently
/// reachable in a test.
pub const RowBytes = union(enum) {
    bytes: u32,
    unsupported_format,
    odd_uyvy_width,
};

/// Minimum bytes in one tightly packed uncompressed row.
///
/// JPEG is not a row format and is reported as unsupported: the caller reaches
/// this only after the compressed branch has been taken.
pub fn rowBytesOf(pixel_format: u8, width: u16) RowBytes {
    if (pixel_format == format.rgb888) {
        return .{ .bytes = @as(u32, width) * 3 };
    }
    if (pixel_format == format.uyvy422) {
        if ((width & 1) != 0) {
            return .odd_uyvy_width;
        }
        return .{ .bytes = @as(u32, width) * 2 };
    }
    return .unsupported_format;
}

/// Every way a frame view can fail validation, in the order the C tests them.
pub const FrameFault = enum {
    ok,
    null_data,
    zero_width,
    zero_height,
    jpeg_zero_bytes,
    jpeg_stride_set,
    unsupported_format,
    odd_uyvy_width,
    stride_short,
    height_overflows_stride,
    bytes_short,
};

/// `ra8_camera_frame_validate` minus its NULL-handle guard, which the ABI owns.
pub fn validateFrame(frame: Frame) FrameFault {
    if (frame.data == null) {
        return .null_data;
    }
    if (frame.width == 0) {
        return .zero_width;
    }
    if (frame.height == 0) {
        return .zero_height;
    }
    if (frame.format == format.jpeg) {
        if (frame.bytes == 0) {
            return .jpeg_zero_bytes;
        }
        if (frame.stride_bytes != 0) {
            return .jpeg_stride_set;
        }
        return .ok;
    }
    const row = switch (rowBytesOf(frame.format, frame.width)) {
        .bytes => |value| value,
        .unsupported_format => return .unsupported_format,
        .odd_uyvy_width => return .odd_uyvy_width,
    };
    if (frame.stride_bytes < row) {
        return .stride_short;
    }
    // `row` is at least 3 for any accepted format and width, so the stride is
    // never zero here and the division below cannot fault.
    if (@as(u32, frame.height) > std.math.maxInt(u32) / frame.stride_bytes) {
        return .height_overflows_stride;
    }
    if (frame.bytes < frame.stride_bytes * @as(u32, frame.height)) {
        return .bytes_short;
    }
    return .ok;
}

/// Capture-buffer rejections, judged after the source handle and before dispatch.
pub const BufferFault = enum { ok, null_data, zero_capacity };

/// The two capture-buffer guards `ra8_camera_source_capture` applies.
pub fn captureBufferFault(has_data: bool, capacity: u32) BufferFault {
    if (!has_data) {
        return .null_data;
    }
    if (capacity == 0) {
        return .zero_capacity;
    }
    return .ok;
}

/// Contract violations only a misbehaving source backend can produce.
pub const CaptureFault = enum { ok, alias_mismatch, bytes_exceed_capacity };

/// The post-dispatch contract: a capture must alias the supplied buffer and
/// must not claim more bytes than the buffer holds.
pub fn capturePostFault(out_addr: usize, buffer_addr: usize, out_bytes: u32, capacity: u32) CaptureFault {
    if (out_addr != buffer_addr) {
        return .alias_mismatch;
    }
    if (out_bytes > capacity) {
        return .bytes_exceed_capacity;
    }
    return .ok;
}

/// Whether a capture buffer can hold the fixed frame a memory source replays.
pub fn memoryCapacityFits(capacity: u32, frame_bytes: u32) bool {
    return capacity >= frame_bytes;
}

/// Legal `ra8_jpeg_sw` quality bounds, mirrored from `inc/ra8_jpeg_sw.h`.
pub const jpeg_quality_min: u8 = 1;
pub const jpeg_quality_max: u8 = 100;

/// Every way a software-JPEG configuration can be rejected, in the C's order.
pub const JpegCfgFault = enum {
    ok,
    null_workspace,
    quality_below_min,
    quality_above_max,
    zero_output_width,
    zero_output_height,
    geometry_overflows,
    workspace_short,
};

/// `ra8_camera_codec_jpeg_sw_init`'s configuration rules.
///
/// The overflow guard is not redundant with the capacity guard: for a 23307 by
/// 61426 output the packed size passes 2^32 by 50 bytes, so a wrapped product
/// would be compared against 50 and accepted.
pub fn validateJpegCfg(
    has_workspace: bool,
    quality: u8,
    output_width: u16,
    output_height: u16,
    workspace_capacity: u32,
) JpegCfgFault {
    if (!has_workspace) {
        return .null_workspace;
    }
    if (quality < jpeg_quality_min) {
        return .quality_below_min;
    }
    if (quality > jpeg_quality_max) {
        return .quality_above_max;
    }
    if (output_width == 0) {
        return .zero_output_width;
    }
    if (output_height == 0) {
        return .zero_output_height;
    }
    const row_bytes = @as(u32, output_width) * 3;
    if (@as(u32, output_height) > std.math.maxInt(u32) / row_bytes) {
        return .geometry_overflows;
    }
    if (workspace_capacity < row_bytes * @as(u32, output_height)) {
        return .workspace_short;
    }
    return .ok;
}

/// Packed-RGB bytes one configured output image occupies. Only meaningful for
/// a configuration ::validateJpegCfg accepted, where it cannot overflow.
pub fn jpegWorkspaceBytes(output_width: u16, output_height: u16) u32 {
    return @as(u32, output_width) * 3 * @as(u32, output_height);
}

/// BT.601 limited-range fixed-point conversion constants.
pub const color = struct {
    pub const luma_black: i32 = 16;
    pub const chroma_mid: i32 = 128;
    pub const luma_scale: i32 = 298;
    pub const red_cr: i32 = 409;
    pub const green_cb: i32 = 100;
    pub const green_cr: i32 = 208;
    pub const blue_cb: i32 = 516;
    pub const rounding: i32 = 128;
    pub const shift: u5 = 8;
    pub const u8_max: i32 = 255;
};

/// Saturate a completed fixed-point channel calculation into one byte.
pub fn clampByte(component: i32) u8 {
    if (component < 0) {
        return 0;
    }
    if (component > color.u8_max) {
        return @intCast(color.u8_max);
    }
    return @intCast(component);
}

/// Convert one limited-range YCbCr pixel to packed RGB888.
pub fn ycbcrToRgb(y: u8, cb: u8, cr: u8) [3]u8 {
    var luma = @as(i32, y) - color.luma_black;
    if (luma < 0) {
        luma = 0;
    }
    const blue_delta = @as(i32, cb) - color.chroma_mid;
    const red_delta = @as(i32, cr) - color.chroma_mid;
    return .{
        clampByte((color.luma_scale * luma + color.red_cr * red_delta + color.rounding) >> color.shift),
        clampByte((color.luma_scale * luma - color.green_cb * blue_delta -
            color.green_cr * red_delta + color.rounding) >> color.shift),
        clampByte((color.luma_scale * luma + color.blue_cb * blue_delta + color.rounding) >> color.shift),
    };
}

/// Byte offset of one packed RGB888 pixel.
///
/// Wrapping arithmetic throughout, because the C's `uint32_t` address maths
/// wraps and a validated frame is what keeps it in range; plain Zig operators
/// would panic instead of reproducing that.
pub fn rgbOffset(stride_bytes: u32, x: u32, y: u32) u32 {
    return (y *% stride_bytes) +% (x *% 3);
}

/// Byte offset of the UYVY chroma pair holding column `x`.
pub fn uyvyPairOffset(stride_bytes: u32, x: u32, y: u32) u32 {
    return (y *% stride_bytes) +% ((x & ~@as(u32, 1)) *% 2);
}

/// Offset of the luma byte inside a UYVY chroma pair: Y0 for an even column,
/// Y1 for an odd one.
pub fn uyvyLumaOffset(x: u32) u32 {
    return if ((x & 1) == 0) 1 else 3;
}

/// Nearest-neighbour source coordinate for one configured output coordinate.
pub fn sourceCoord(out_index: u32, source_extent: u16, out_extent: u32) u32 {
    return (out_index *% @as(u32, source_extent)) / out_extent;
}

/// Byte offset of one pixel in the packed-RGB workspace.
pub fn workspaceOffset(x: u32, y: u32, out_width: u32) usize {
    return ((@as(usize, y) * @as(usize, out_width)) + @as(usize, x)) * 3;
}

/// Read one source pixel as packed RGB888, decoding UYVY through its shared
/// chroma pair when the input is not already RGB.
pub fn readRgb(data: [*]const u8, pixel_format: u8, stride_bytes: u32, x: u32, y: u32) [3]u8 {
    if (pixel_format == format.rgb888) {
        const offset = rgbOffset(stride_bytes, x, y);
        return .{ data[offset], data[offset + 1], data[offset + 2] };
    }
    const pair = uyvyPairOffset(stride_bytes, x, y);
    const luma = uyvyLumaOffset(x);
    return ycbcrToRgb(data[pair + luma], data[pair], data[pair + 2]);
}

/// Nearest-neighbour sample a whole input frame into the packed-RGB workspace.
pub fn prepareRgb(
    data: [*]const u8,
    pixel_format: u8,
    stride_bytes: u32,
    in_width: u16,
    in_height: u16,
    workspace: [*]u8,
    out_width: u16,
    out_height: u16,
) void {
    const columns: u32 = out_width;
    const rows: u32 = out_height;
    var y: u32 = 0;
    while (y < rows) : (y += 1) {
        const source_y = sourceCoord(y, in_height, rows);
        var x: u32 = 0;
        while (x < columns) : (x += 1) {
            const source_x = sourceCoord(x, in_width, columns);
            const pixel = readRgb(data, pixel_format, stride_bytes, source_x, source_y);
            const offset = workspaceOffset(x, y, columns);
            workspace[offset] = pixel[0];
            workspace[offset + 1] = pixel[1];
            workspace[offset + 2] = pixel[2];
        }
    }
}
