//! Unit tests for deterministic OCR image preprocessing and Helium MVE parity.

const std = @import("std");
const ocr = @import("ocr");

fn expectedAffineI8(val: u8, input_scale: f32, mean: f32, inv_std: f32, qscale: f32, zp: i8) i8 {
    const f_val: f32 = @floatFromInt(val);
    const norm: f32 = (f_val * input_scale - mean) * inv_std;
    const qf: f32 = norm / qscale + @as(f32, @floatFromInt(zp));
    const clamped: f32 = std.math.clamp(qf, @as(f32, -128.0), @as(f32, 127.0));
    return @intFromFloat(@round(clamped));
}

test "OCR preprocessing rejects invalid geometry, stride, crop bounds, and quant args" {
    const img: [16]u8 = @splat(10);
    var out_u8: [16]u8 = @splat(0x5A);
    var out_i8: [16]i8 = @splat(-77);

    const bad_spec: ocr.ImageSpec = .{ .stride_bytes = 3, .width = 4, .height = 4, .format = 1 };
    try std.testing.expectError(
        error.InvalidArg,
        ocr.resizeCropRotateU8(&img, bad_spec, .{ .x = 0, .y = 0, .width = 2, .height = 2 }, .{ .dst_width = 2, .dst_height = 2 }, &out_u8),
    );

    const spec: ocr.ImageSpec = .{ .stride_bytes = 4, .width = 4, .height = 4, .format = 1 };
    try std.testing.expectError(
        error.OutOfRange,
        ocr.resizeCropRotateU8(&img, spec, .{ .x = 3, .y = 0, .width = 2, .height = 2 }, .{ .dst_width = 2, .dst_height = 2 }, &out_u8),
    );

    try std.testing.expectError(
        error.InvalidSize,
        ocr.resizeCropRotateU8(&img, spec, .{ .x = 0, .y = 0, .width = 4, .height = 4 }, .{ .dst_width = 4, .dst_height = 4 }, out_u8[0..8]),
    );
    try std.testing.expectEqual(@as(u8, 0x5A), out_u8[0]);

    const nan_q: ocr.QuantParams = .{ .mean = .{ std.math.nan(f32), 0.0, 0.0 } };
    try std.testing.expectError(
        error.InvalidArg,
        ocr.preprocessCropRotateToI8(&img, spec, .{ .x = 0, .y = 0, .width = 4, .height = 4 }, .{ .dst_width = 2, .dst_height = 2 }, nan_q, &out_i8),
    );
    try std.testing.expectEqual(@as(i8, -77), out_i8[0]);

    const zero_std_q: ocr.QuantParams = .{ .inv_std = .{ 0.0, 1.0, 1.0 } };
    try std.testing.expectError(
        error.InvalidArg,
        ocr.normalizeU8ToI8(&img, 1, zero_std_q, &out_i8),
    );
    try std.testing.expectError(
        error.InvalidArg,
        ocr.requiredDstBytes(0, 4, 1),
    );
}

test "OCR crop and bilinear resize keep border isolation and match coordinate modes" {
    // 4x4 grayscale image where crop [1..3] x [1..3] holds {10, 20; 30, 40} surrounded by 255.
    const img = [16]u8{
        255, 255, 255, 255,
        255, 10,  20,  255,
        255, 30,  40,  255,
        255, 255, 255, 255,
    };
    const spec: ocr.ImageSpec = .{ .stride_bytes = 5, .width = 4, .height = 4, .format = 1 };
    const padded_img = [20]u8{
        255, 255, 255, 255, 99,
        255, 10,  20,  255, 99,
        255, 30,  40,  255, 99,
        255, 255, 255, 255, 99,
    };
    const crop: ocr.Rect = .{ .x = 1, .y = 1, .width = 2, .height = 2 };
    _ = img;

    // 1x1 output under half_pixel must sample exact center of {10, 20, 30, 40} = 25 (no bleed from 255).
    var one: [1]u8 = @splat(0);
    const n1 = try ocr.resizeCropRotateU8(
        &padded_img,
        spec,
        crop,
        .{ .dst_width = 1, .dst_height = 1, .coord_mode = @backingInt(ocr.CoordMode.half_pixel) },
        &one,
    );
    try std.testing.expectEqual(@as(usize, 1), n1);
    try std.testing.expectEqual(@as(u8, 25), one[0]);

    // 3x3 output under align_corners maps corners to 10, 20, 30, 40 and midpoints to 15, 25, 35.
    var aligned: [9]u8 = @splat(0);
    _ = try ocr.resizeCropRotateU8(
        &padded_img,
        spec,
        crop,
        .{ .dst_width = 3, .dst_height = 3, .coord_mode = @backingInt(ocr.CoordMode.align_corners) },
        &aligned,
    );
    try std.testing.expectEqualSlices(u8, &[_]u8{ 10, 15, 20, 20, 25, 30, 30, 35, 40 }, &aligned);

    // 1:1 identity resize reproduces crop verbatim even with stride > width.
    var ident: [4]u8 = @splat(0);
    _ = try ocr.resizeCropRotateU8(
        &padded_img,
        spec,
        crop,
        .{ .dst_width = 2, .dst_height = 2, .coord_mode = @backingInt(ocr.CoordMode.asym_scale) },
        &ident,
    );
    try std.testing.expectEqualSlices(u8, &[_]u8{ 10, 20, 30, 40 }, &ident);
}

test "OCR vertical CJK column rotates cleanly 90 degrees CCW into horizontal LTR line" {
    // Vertical column crop: width=2, height=3.
    // Top row has glyph A pixels (10, 11), middle (20, 21), bottom has glyph B (30, 31).
    const src = [_]u8{
        10, 11,
        20, 21,
        30, 31,
    };
    const spec: ocr.ImageSpec = .{ .stride_bytes = 2, .width = 2, .height = 3, .format = 1 };
    const crop: ocr.Rect = .{ .x = 0, .y = 0, .width = 2, .height = 3 };
    try std.testing.expectEqual(ocr.Rotation.ccw_90, ocr.suggestVerticalRotation(crop));

    var ccw: [6]u8 = @splat(0);
    _ = try ocr.resizeCropRotateU8(
        &src,
        spec,
        crop,
        .{
            .dst_width = 3,
            .dst_height = 2,
            .rotation = @backingInt(ocr.Rotation.ccw_90),
            .coord_mode = @backingInt(ocr.CoordMode.align_corners),
        },
        &ccw,
    );
    // Top row (10, 11) lands on the left column (dx=0), right side up -> (11, 21, 31 / 10, 20, 30).
    try std.testing.expectEqualSlices(u8, &[_]u8{ 11, 21, 31, 10, 20, 30 }, &ccw);

    var cw: [6]u8 = @splat(0);
    _ = try ocr.resizeCropRotateU8(
        &src,
        spec,
        crop,
        .{
            .dst_width = 3,
            .dst_height = 2,
            .rotation = @backingInt(ocr.Rotation.cw_90),
            .coord_mode = @backingInt(ocr.CoordMode.align_corners),
        },
        &cw,
    );
    try std.testing.expectEqualSlices(u8, &[_]u8{ 30, 20, 10, 31, 21, 11 }, &cw);

    var r180: [6]u8 = @splat(0);
    _ = try ocr.resizeCropRotateU8(
        &src,
        spec,
        crop,
        .{
            .dst_width = 2,
            .dst_height = 3,
            .rotation = @backingInt(ocr.Rotation.rot_180),
            .coord_mode = @backingInt(ocr.CoordMode.align_corners),
        },
        &r180,
    );
    try std.testing.expectEqualSlices(u8, &[_]u8{ 31, 30, 21, 20, 11, 10 }, &r180);
}

test "OCR channel conversion handles gray8 <-> rgb888 deterministically" {
    const gray = [_]u8{ 0, 128, 255, 64 };
    const gray_spec: ocr.ImageSpec = .{ .stride_bytes = 2, .width = 2, .height = 2, .format = 1 };
    var rgb_out: [12]u8 = @splat(0);
    _ = try ocr.resizeCropRotateU8(
        &gray,
        gray_spec,
        .{ .x = 0, .y = 0, .width = 2, .height = 2 },
        .{ .dst_width = 2, .dst_height = 2, .dst_format = @backingInt(ocr.PixelFormat.rgb888) },
        &rgb_out,
    );
    try std.testing.expectEqualSlices(
        u8,
        &[_]u8{ 0, 0, 0, 128, 128, 128, 255, 255, 255, 64, 64, 64 },
        &rgb_out,
    );

    const rgb_spec: ocr.ImageSpec = .{ .stride_bytes = 6, .width = 2, .height = 2, .format = 3 };
    const rgb_in = [_]u8{
        255, 0, 0,   0,   255, 0,
        0,   0, 255, 200, 200, 200,
    };
    var gray_out: [4]u8 = @splat(0);
    _ = try ocr.resizeCropRotateU8(
        &rgb_in,
        rgb_spec,
        .{ .x = 0, .y = 0, .width = 2, .height = 2 },
        .{ .dst_width = 2, .dst_height = 2, .dst_format = @backingInt(ocr.PixelFormat.gray8) },
        &gray_out,
    );
    // BT.601 integer luma: R->(77*255+128)>>8=77, G->149, B->29, (200,200,200)->200.
    try std.testing.expectEqualSlices(u8, &[_]u8{ 77, 149, 29, 200 }, &gray_out);
}

test "OCR SIMD vector path matches scalar normalization bit-for-bit across sizes" {
    const quant: ocr.QuantParams = .{
        .mean = .{ 0.485, 0.456, 0.406 },
        .inv_std = .{ 1.0 / 0.229, 1.0 / 0.224, 1.0 / 0.225 },
        .input_scale = 1.0 / 255.0,
        .quant_scale = 0.025,
        .zero_point = -14,
    };

    const lengths = [_]usize{ 1, 3, 4, 15, 16, 17, 37, 64 };
    var src_buf: [64]u8 = undefined;
    for (&src_buf, 0..) |*b, idx| {
        b.* = @intCast((idx * 41 + 7) % 256);
    }

    var scalar_out: [64]i8 = undefined;
    var simd_out: [64]i8 = undefined;
    for (lengths) |n| {
        const s_len = try ocr.normalizeU8ToI8WithMode(src_buf[0..n], 1, quant, scalar_out[0..n], false);
        const v_len = try ocr.normalizeU8ToI8WithMode(src_buf[0..n], 1, quant, simd_out[0..n], true);
        try std.testing.expectEqual(n, s_len);
        try std.testing.expectEqual(n, v_len);
        try std.testing.expectEqualSlices(i8, scalar_out[0..n], simd_out[0..n]);
    }

    const rgb_lengths = [_]usize{ 3, 6, 12, 15, 24, 39, 63 };
    for (rgb_lengths) |n| {
        _ = try ocr.normalizeU8ToI8WithMode(src_buf[0..n], 3, quant, scalar_out[0..n], false);
        _ = try ocr.normalizeU8ToI8WithMode(src_buf[0..n], 3, quant, simd_out[0..n], true);
        try std.testing.expectEqualSlices(i8, scalar_out[0..n], simd_out[0..n]);
    }

    // Half-away-from-zero ties (-4.5 -> -5, +4.5 -> +5) and int8 saturation (-128 / +127).
    const tie_q: ocr.QuantParams = .{
        .mean = .{ 0.0, 0.0, 0.0 },
        .inv_std = .{ 1.0, 1.0, 1.0 },
        .input_scale = 1.0,
        .quant_scale = 2.0,
        .zero_point = -10,
    };
    const tie_in = [_]u8{ 0, 11, 29, 255 };
    var tie_scalar: [4]i8 = undefined;
    var tie_simd: [4]i8 = undefined;
    _ = try ocr.normalizeU8ToI8WithMode(&tie_in, 1, tie_q, &tie_scalar, false);
    _ = try ocr.normalizeU8ToI8WithMode(&tie_in, 1, tie_q, &tie_simd, true);
    try std.testing.expectEqualSlices(i8, &[_]i8{ -10, -5, 5, 118 }, &tie_scalar);
    try std.testing.expectEqualSlices(i8, &tie_scalar, &tie_simd);
}

test "OCR end-to-end streaming preprocess produces deterministic int8 tensor vectors" {
    // 6x2 RGB gradient strip streamed through fused crop->rotate->resize->quantize (>192 bytes output).
    const spec: ocr.ImageSpec = .{ .stride_bytes = 18, .width = 6, .height = 2, .format = 3 };
    var img: [36]u8 = undefined;
    for (&img, 0..) |*p, idx| {
        p.* = @intCast((idx * 19 + 11) % 256);
    }
    const rect: ocr.Rect = .{ .x = 1, .y = 0, .width = 4, .height = 2 };
    const cfg: ocr.PreprocessCfg = .{
        .dst_width = 10,
        .dst_height = 8,
        .dst_format = @backingInt(ocr.PixelFormat.rgb888),
        .rotation = @backingInt(ocr.Rotation.none),
        .coord_mode = @backingInt(ocr.CoordMode.half_pixel),
    };
    const quant: ocr.QuantParams = .{
        .mean = .{ 0.5, 0.5, 0.5 },
        .inv_std = .{ 2.0, 2.0, 2.0 },
        .input_scale = 1.0 / 255.0,
        .quant_scale = 1.0 / 128.0,
        .zero_point = 0,
    };

    var ref_u8: [240]u8 = undefined;
    const u8_bytes = try ocr.resizeCropRotateU8(&img, spec, rect, cfg, &ref_u8);
    try std.testing.expectEqual(@as(usize, 240), u8_bytes);

    var fused_i8: [240]i8 = undefined;
    const fused_bytes = try ocr.preprocessCropRotateToI8(&img, spec, rect, cfg, quant, &fused_i8);
    try std.testing.expectEqual(@as(usize, 240), fused_bytes);

    for (ref_u8, 0..) |px, idx| {
        const ch = idx % 3;
        const exp = expectedAffineI8(
            px,
            quant.input_scale,
            quant.mean[ch],
            quant.inv_std[ch],
            quant.quant_scale,
            quant.zero_point,
        );
        try std.testing.expectEqual(exp, fused_i8[idx]);
    }
}
