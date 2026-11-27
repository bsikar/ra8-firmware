//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! End-to-end Zig ABI verification of real `.tchr`, Q16 preprocessing, DB
//! detector post-processing, and CTC int8 decode goldens against `ra8_translate`.

const std = @import("std");
const goldens = @import("goldens");

const ra8_ok: u16 = 0;
const format_rgb888: u8 = 3;
const coord_half_pixel: u8 = 0;
const rot_none: u8 = 0;
const rot_ccw_90: u8 = 1;
const det_order_manga_rtl: u8 = 0;
const det_flag_drop_furigana: u8 = 1;
const ctc_score_logits: u8 = 0;

const Rect = extern struct {
    x: u16 = 0,
    y: u16 = 0,
    width: u16 = 0,
    height: u16 = 0,
};

const ImageSpec = extern struct {
    stride_bytes: u32 = 0,
    width: u16 = 0,
    height: u16 = 0,
    format: u8 = format_rgb888,
    reserved: [3]u8 = .{ 0, 0, 0 },
};

const PreprocessCfg = extern struct {
    dst_width: u16 = 0,
    dst_height: u16 = 0,
    dst_format: u8 = format_rgb888,
    rotation: u8 = rot_none,
    coord_mode: u8 = coord_half_pixel,
    reserved: u8 = 0,
};

const QuantParams = extern struct {
    mean: [3]f32 = .{ 0.0, 0.0, 0.0 },
    inv_std: [3]f32 = .{ 1.0, 1.0, 1.0 },
    input_scale: f32 = 1.0 / 255.0,
    quant_scale: f32 = 1.0 / 128.0,
    zero_point: i8 = 0,
    reserved: [3]u8 = .{ 0, 0, 0 },
};

const DetLine = extern struct {
    x: u16 = 0,
    y: u16 = 0,
    w: u16 = 0,
    h: u16 = 0,
    bubble_index: u16 = 0,
    confidence_bps: u16 = 0,
    text_orientation: u8 = 0,
    flags: u8 = 0,
    order_in_bubble: u16 = 0,
};

const DetBubble = extern struct {
    x: u16 = 0,
    y: u16 = 0,
    w: u16 = 0,
    h: u16 = 0,
    reading_order: u16 = 0,
    confidence_bps: u16 = 0,
    first_line: u16 = 0,
    line_count: u16 = 0,
    shape: u8 = 0,
    text_orientation: u8 = 0,
    flags: u8 = 0,
    reserved: u8 = 0,
};

const DetCfg = extern struct {
    map_width: u16,
    map_height: u16,
    row_stride: u16 = 0,
    elem_stride: u16 = 0,
    page_width: u16,
    page_height: u16,
    text_thresh_bps: u16 = 3000,
    bubble_thresh_bps: u16 = 4000,
    min_comp_area: u16 = 1,
    box_pad_pct: u16 = 0,
    group_gap_pct: u16 = 150,
    furigana_max_ratio_pct: u16 = 55,
    furigana_max_gap_pct: u16 = 120,
    reading_order: u8,
    flags: u8,
    max_components: u16,
    reserved: [2]u8 = .{ 0, 0 },
};

const DetSummary = extern struct {
    raw_components: u32 = 0,
    dropped_furigana: u32 = 0,
    line_count: u32 = 0,
    bubble_count: u32 = 0,
};

const CtcCharsetHeader = extern struct {
    total_bytes: u32 = 0,
    container_crc32: u32 = 0,
    version: u16 = 0,
    flags: u16 = 0,
    class_count: u32 = 0,
    blank_index: u32 = 0,
    unk_index: u32 = 0,
    string_pool_bytes: u32 = 0,
    ocr_model: u8 = 0,
    reserved: [3]u8 = .{ 0, 0, 0 },
};

const CtcDecodeCfg = extern struct {
    time_steps: u32,
    class_stride: u32 = 0,
    score_mode: u8 = ctc_score_logits,
    decode_flags: u8 = 0,
    low_confidence_thresh_bps: u16 = 7500,
    expected_ocr_model: u8,
    reserved: [3]u8 = .{ 0, 0, 0 },
};

const CtcResult = extern struct {
    text_bytes: u32 = 0,
    glyph_count: u32 = 0,
    unknown_count: u32 = 0,
    collapsed_steps: u32 = 0,
    confidence_bps: u16 = 0,
    min_char_confidence_bps: u16 = 0,
    low_confidence: u8 = 0,
    reserved: [3]u8 = .{ 0, 0, 0 },
};

extern fn ra8_translate_ocr_preprocess_i8(
    src_opt: ?[*]const u8,
    src_len: usize,
    src_spec_opt: ?*const ImageSpec,
    crop_opt: ?*const Rect,
    cfg_opt: ?*const PreprocessCfg,
    quant_opt: ?*const QuantParams,
    out_dst_opt: ?[*]i8,
    out_cap: usize,
    out_written_opt: ?*usize,
) callconv(.c) u16;

extern fn ra8_translate_det_work_bytes(
    map_width: u16,
    max_components: u16,
    out_work_bytes: *usize,
) callconv(.c) u16;

extern fn ra8_translate_det_post_i8(
    text_map: [*]const i8,
    text_elem_count: usize,
    text_scale: f32,
    text_zp: i8,
    bubble_map: ?[*]const i8,
    bubble_elem_count: usize,
    bubble_scale: f32,
    bubble_zp: i8,
    cfg: *const DetCfg,
    work_buf: [*]u8,
    work_cap: usize,
    out_lines: [*]DetLine,
    line_cap: usize,
    out_bubbles: [*]DetBubble,
    bubble_cap: usize,
    out_summary: *DetSummary,
) callconv(.c) u16;

extern fn ra8_translate_ctc_charset_open(
    tchr_buf: [*]const u8,
    tchr_len: usize,
    out_header: *CtcCharsetHeader,
) callconv(.c) u16;

extern fn ra8_translate_ctc_decode_i8(
    tchr_buf: [*]const u8,
    tchr_len: usize,
    logits_i8: [*]const i8,
    logit_elem_count: usize,
    quant_scale: f32,
    zero_point: i8,
    cfg: *const CtcDecodeCfg,
    out_utf8: [*]u8,
    out_cap: usize,
    out_result: *CtcResult,
) callconv(.c) u16;

test "detector preprocessing CRC and post-processing extract lines from real int8 map" {
    const dg = goldens.det_golden;
    const dst_len: usize = 256 * 256 * 3;
    const dst_i8 = try std.testing.allocator.alloc(i8, dst_len);
    defer std.testing.allocator.free(dst_i8);

    const spec: ImageSpec = .{
        .stride_bytes = @as(u32, dg.src_width) * 3,
        .width = dg.src_width,
        .height = dg.src_height,
        .format = format_rgb888,
    };
    const crop: Rect = .{ .x = dg.crop_x, .y = dg.crop_y, .width = dg.crop_w, .height = dg.crop_h };
    const cfg: PreprocessCfg = .{
        .dst_width = 256,
        .dst_height = 256,
        .dst_format = format_rgb888,
        .rotation = rot_none,
        .coord_mode = coord_half_pixel,
    };
    const quant: QuantParams = .{
        .mean = .{ 0.485, 0.456, 0.406 },
        .inv_std = .{ 1.0 / 0.229, 1.0 / 0.224, 1.0 / 0.225 },
        .input_scale = 1.0 / 255.0,
        .quant_scale = dg.in_scale,
        .zero_point = dg.in_zp,
    };
    var written: usize = 0;
    try std.testing.expectEqual(
        ra8_ok,
        ra8_translate_ocr_preprocess_i8(
            dg.src_rgb.ptr,
            dg.src_rgb.len,
            &spec,
            &crop,
            &cfg,
            &quant,
            dst_i8.ptr,
            dst_i8.len,
            &written,
        ),
    );
    try std.testing.expectEqual(dst_len, written);
    const crc = std.hash.Crc32.hash(std.mem.sliceAsBytes(dst_i8));
    try std.testing.expectEqual(dg.out_i8.len, 256 * 256);
    try std.testing.expectEqual(dg.in_i8_crc32, crc);
    try std.testing.expectEqual(dg.out_i8_crc32, std.hash.Crc32.hash(dg.out_i8));

    const max_comps: u16 = 32;
    var work_bytes: usize = 0;
    try std.testing.expectEqual(ra8_ok, ra8_translate_det_work_bytes(256, max_comps, &work_bytes));
    const work_buf = try std.testing.allocator.alloc(u8, work_bytes);
    defer std.testing.allocator.free(work_buf);
    @memset(work_buf, 0);

    var lines: [32]DetLine = @splat(.{});
    var bubbles: [16]DetBubble = @splat(.{});
    var summary: DetSummary = .{};
    const det_cfg: DetCfg = .{
        .map_width = 256,
        .map_height = 256,
        .page_width = 256,
        .page_height = 256,
        .reading_order = det_order_manga_rtl,
        .flags = det_flag_drop_furigana,
        .max_components = max_comps,
    };
    const out_i8: []const i8 = @ptrCast(dg.out_i8);
    try std.testing.expectEqual(
        ra8_ok,
        ra8_translate_det_post_i8(
            out_i8.ptr,
            out_i8.len,
            dg.out_scale,
            dg.out_zp,
            null,
            0,
            1.0,
            0,
            &det_cfg,
            work_buf.ptr,
            work_buf.len,
            &lines,
            lines.len,
            &bubbles,
            bubbles.len,
            &summary,
        ),
    );
    try std.testing.expect(summary.line_count >= 2);
    try std.testing.expect(summary.bubble_count >= 1);
}

test "recognizer preprocessing CRC and CTC int8 decoding match golden UTF-8 outputs" {
    for (goldens.rec_goldens) |rg| {
        const dst_len: usize = @as(usize, rg.dst_width) * @as(usize, rg.dst_height) * 3;
        const dst_i8 = try std.testing.allocator.alloc(i8, dst_len);
        defer std.testing.allocator.free(dst_i8);

        const spec: ImageSpec = .{
            .stride_bytes = @as(u32, rg.src_width) * 3,
            .width = rg.src_width,
            .height = rg.src_height,
            .format = format_rgb888,
        };
        const crop: Rect = .{ .x = rg.crop_x, .y = rg.crop_y, .width = rg.crop_w, .height = rg.crop_h };
        const cfg: PreprocessCfg = .{
            .dst_width = rg.dst_width,
            .dst_height = rg.dst_height,
            .dst_format = format_rgb888,
            .rotation = if (rg.vertical_ccw90) rot_ccw_90 else rot_none,
            .coord_mode = coord_half_pixel,
        };
        const quant: QuantParams = .{
            .mean = .{ 0.5, 0.5, 0.5 },
            .inv_std = .{ 2.0, 2.0, 2.0 },
            .input_scale = 1.0 / 255.0,
            .quant_scale = rg.in_scale,
            .zero_point = rg.in_zp,
        };
        var written: usize = 0;
        try std.testing.expectEqual(
            ra8_ok,
            ra8_translate_ocr_preprocess_i8(
                rg.src_rgb.ptr,
                rg.src_rgb.len,
                &spec,
                &crop,
                &cfg,
                &quant,
                dst_i8.ptr,
                dst_i8.len,
                &written,
            ),
        );
        try std.testing.expectEqual(dst_len, written);
        const crc = std.hash.Crc32.hash(std.mem.sliceAsBytes(dst_i8));
        try std.testing.expectEqual(rg.in_i8_crc32, crc);

        var hdr: CtcCharsetHeader = .{};
        try std.testing.expectEqual(ra8_ok, ra8_translate_ctc_charset_open(rg.tchr.ptr, rg.tchr.len, &hdr));
        try std.testing.expectEqual(rg.ocr_model, hdr.ocr_model);
        try std.testing.expectEqual(rg.out_classes, hdr.class_count);

        const logits: []const i8 = @ptrCast(rg.logits_i8);
        const ctc_cfg: CtcDecodeCfg = .{
            .time_steps = rg.out_steps,
            .class_stride = rg.out_classes,
            .score_mode = ctc_score_logits,
            .expected_ocr_model = rg.ocr_model,
        };
        var out_utf8: [256]u8 = undefined;
        var res: CtcResult = .{};
        try std.testing.expectEqual(
            ra8_ok,
            ra8_translate_ctc_decode_i8(
                rg.tchr.ptr,
                rg.tchr.len,
                logits.ptr,
                logits.len,
                rg.out_scale,
                rg.out_zp,
                &ctc_cfg,
                &out_utf8,
                out_utf8.len,
                &res,
            ),
        );
        try std.testing.expectEqualStrings(rg.expected_utf8, out_utf8[0..res.text_bytes]);
    }
}
