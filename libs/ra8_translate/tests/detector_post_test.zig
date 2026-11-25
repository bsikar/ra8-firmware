//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Unit and C ABI tests for DB detector post-processing: thresholding,
//! streaming connected components, furigana suppression, bubble grouping,
//! and manga RTL vs webtoon TTB reading order.

const std = @import("std");
const abi = @import("detector_post_abi");
const det = @import("detector_post");
const err_mod = @import("err");

fn paintRect(map: []f32, w: usize, x0: usize, y0: usize, rw: usize, rh: usize, val: f32) void {
    var y = y0;
    while (y < y0 + rh) : (y += 1) {
        var x = x0;
        while (x < x0 + rw) : (x += 1) {
            map[y * w + x] = val;
        }
    }
}

test "streaming connected components merge U-shapes and switch between 8-connected and 4-connected" {
    // 10x8 map with:
    // 1) A U-shape component (x=1..3, y=1..4) -> 1 box
    // 2) Two diagonal pixels at (7,1) and (8,2) -> 1 box in 8-conn, 2 boxes in 4-conn
    // 3) Single noise pixel at (9,7) rejected by min_comp_area=2
    const w: usize = 10;
    const h: usize = 8;
    var map: [w * h]f32 = @splat(0.0);

    // U-shape
    paintRect(&map, w, 1, 1, 1, 4, 0.9);
    paintRect(&map, w, 1, 4, 3, 1, 0.8);
    paintRect(&map, w, 3, 1, 1, 4, 0.9);

    // Diagonal pair expanded to 2x2 diagonal touch for min_comp_area=2 in 4-conn or area=1 for diagonal check
    map[1 * w + 7] = 0.85;
    map[2 * w + 8] = 0.85;
    map[7 * w + 9] = 0.95; // noise area=1

    var work: [4096]u8 = undefined;
    var lines: [8]det.DetLine = undefined;
    var bubbles: [8]det.DetBubble = undefined;

    const sum8 = try det.postProcessF32(&map, null, .{
        .map_width = w,
        .map_height = h,
        .page_width = 100,
        .page_height = 80,
        .min_comp_area = 2,
        .group_gap_pct = 0,
        .flags = 0,
    }, &work, &lines, &bubbles);
    try std.testing.expectEqual(@as(u32, 2), sum8.raw_components);
    try std.testing.expectEqual(@as(u32, 2), sum8.line_count);
    // U-shape scales x=[1..4) -> [10..40), y=[1..5) -> [10..50)
    try std.testing.expectEqual(@as(u16, 10), lines[1].x);
    try std.testing.expectEqual(@as(u16, 10), lines[1].y);
    try std.testing.expectEqual(@as(u16, 30), lines[1].w);
    try std.testing.expectEqual(@as(u16, 40), lines[1].h);

    // With 4-connectivity and min_comp_area=1, diagonals become separate plus bottom-right pixel stays.
    const sum4 = try det.postProcessF32(&map, null, .{
        .map_width = w,
        .map_height = h,
        .page_width = 100,
        .page_height = 80,
        .min_comp_area = 1,
        .group_gap_pct = 0,
        .flags = det.flag_4_connected,
    }, &work, &lines, &bubbles);
    try std.testing.expectEqual(@as(u32, 4), sum4.raw_components);
    try std.testing.expectEqual(@as(u32, 4), sum4.line_count);
}

test "furigana suppression drops small strips next to kanji columns while preserving standalone boxes" {
    // 40x40 map mapped 10x -> 400x400 page.
    const w: usize = 40;
    const h: usize = 40;
    var map: [w * h]f32 = @splat(0.0);

    // Kanji col 1 (x=28..31 => x=280, w=30; y=4..24 => y=40, h=200)
    paintRect(&map, w, 28, 4, 3, 20, 0.9);
    // Adjacent furigana to its right (x=32..33 => x=320, w=10 = 33% of 30; y=6..14 => overlap 100%, gap=10 <= 36)
    paintRect(&map, w, 32, 6, 1, 8, 0.85);
    // Adjacent sibling kanji col 2 on left (x=24..27 => w=30, 100% width -> NOT furigana)
    paintRect(&map, w, 24, 4, 3, 20, 0.9);

    // Isolated tiny "!" bubble far away at (4, 30, 1x5 => 10x50): NO neighbor -> MUST stay!
    paintRect(&map, w, 4, 30, 1, 5, 0.95);

    const work_bytes = try det.minWorkBytes(w, 16);
    var work: [8192]u8 = undefined;
    try std.testing.expect(work_bytes <= work.len);

    var lines: [16]det.DetLine = undefined;
    var bubbles: [16]det.DetBubble = undefined;
    const sum = try det.postProcessF32(&map, null, .{
        .map_width = w,
        .map_height = h,
        .page_width = 400,
        .page_height = 400,
        .reading_order = @backingInt(det.ReadingOrder.manga_rtl),
        .flags = det.flag_drop_furigana,
        .group_gap_pct = 150,
    }, work[0..work_bytes], &lines, &bubbles);

    try std.testing.expectEqual(@as(u32, 4), sum.raw_components);
    try std.testing.expectEqual(@as(u32, 1), sum.dropped_furigana);
    try std.testing.expectEqual(@as(u32, 3), sum.line_count);
    try std.testing.expectEqual(@as(u32, 2), sum.bubble_count);

    // Bubble 0 (top-right): merges sibling kanji cols (x=240..310); right line comes first!
    try std.testing.expectEqual(@as(u16, 2), bubbles[0].line_count);
    try std.testing.expectEqual(@as(u16, 0), bubbles[0].first_line);
    try std.testing.expectEqual(@as(u8, 1), bubbles[0].text_orientation);
    try std.testing.expectEqual(@as(u16, 280), lines[0].x);
    try std.testing.expectEqual(@as(u16, 240), lines[1].x);

    // Bubble 1 (bottom-left standalone "!"): kept!
    try std.testing.expectEqual(@as(u16, 1), bubbles[1].line_count);
    try std.testing.expectEqual(@as(u16, 40), lines[2].x);
    try std.testing.expectEqual(@as(u16, 300), lines[2].y);
}

test "manga RTL vs webtoon TTB reading order and bubble_map grouping work deterministically" {
    const w: usize = 32;
    const h: usize = 32;
    var txt: [w * h]f32 = @splat(0.0);
    var bub: [w * h]f32 = @splat(0.0);

    // Bubble Top-Left (x=2..8, y=2..10): horizontal lines at y=3..5 and y=7..9 (gap > minor without bub map, grouped via bub map!)
    paintRect(&bub, w, 2, 2, 8, 9, 0.9);
    paintRect(&txt, w, 3, 3, 6, 2, 0.9);
    paintRect(&txt, w, 3, 7, 6, 2, 0.9);

    // Bubble Top-Right (x=20..28, y=3..11): 1 box
    paintRect(&bub, w, 20, 3, 8, 8, 0.9);
    paintRect(&txt, w, 22, 4, 4, 6, 0.92);

    // Bubble Bottom (x=14..22, y=20..28): 1 box
    paintRect(&txt, w, 14, 20, 6, 6, 0.88);

    const needed = try det.minWorkBytes(w, 8);
    var work: [8192]u8 = undefined;
    try std.testing.expect(needed <= work.len);
    var lines: [8]det.DetLine = undefined;
    var bubbles: [8]det.DetBubble = undefined;

    // Manga RTL: Top-Right -> Top-Left -> Bottom
    const m_sum = try det.postProcessF32(&txt, &bub, .{
        .map_width = w,
        .map_height = h,
        .page_width = 320,
        .page_height = 320,
        .reading_order = @backingInt(det.ReadingOrder.manga_rtl),
        .flags = 0,
        .max_components = 8,
    }, work[0..needed], &lines, &bubbles);
    try std.testing.expectEqual(@as(u32, 3), m_sum.bubble_count);
    try std.testing.expectEqual(@as(u16, 200), bubbles[0].x); // Top-Right first!
    try std.testing.expectEqual(@as(u16, 1), bubbles[0].flags); // Backed by bubble_map
    try std.testing.expectEqual(@as(u16, 20), bubbles[1].x); // Top-Left second!
    try std.testing.expectEqual(@as(u16, 2), bubbles[1].line_count);
    try std.testing.expectEqual(@as(u16, 30), lines[bubbles[1].first_line].y);
    try std.testing.expectEqual(@as(u16, 70), lines[bubbles[1].first_line + 1].y);
    try std.testing.expectEqual(@as(u16, 140), bubbles[2].x); // Bottom third!
    try std.testing.expectEqual(@as(u16, 0), bubbles[2].flags); // Spatial fallback

    // Webtoon TTB: Top-Left (y=20) -> Top-Right (y=30) -> Bottom (y=200)
    const w_sum = try det.postProcessF32(&txt, &bub, .{
        .map_width = w,
        .map_height = h,
        .page_width = 320,
        .page_height = 320,
        .reading_order = @backingInt(det.ReadingOrder.webtoon_ttb),
        .flags = 0,
        .max_components = 8,
    }, work[0..needed], &lines, &bubbles);
    try std.testing.expectEqual(@as(u32, 3), w_sum.bubble_count);
    try std.testing.expectEqual(@as(u16, 20), bubbles[0].x);
    try std.testing.expectEqual(@as(u16, 200), bubbles[1].x);
    try std.testing.expectEqual(@as(u16, 140), bubbles[2].x);
}

test "strided NHWC int8 quantization matches float post-processing and enforces C ABI error guards" {
    const w: usize = 12;
    const h: usize = 10;
    // 2-channel interleaved [H, W, 2] with row padding (+2 bytes per row => stride = 26)
    const row_stride: usize = w * 2 + 2;
    var nhwc_i8: [h * row_stride]i8 = @splat(-128);
    var bub_ref: [w * h]f32 = @splat(0.0);
    var txt_ref: [w * h]f32 = @splat(0.0);

    // Channel 0 = text, Channel 1 = bubble, scale = 1/200 = 0.005, zp = -100 => p=0.9 -> q=80
    var y: usize = 2;
    while (y < 6) : (y += 1) {
        var x: usize = 2;
        while (x < 6) : (x += 1) {
            nhwc_i8[y * row_stride + x * 2 + 0] = 80;
            nhwc_i8[y * row_stride + x * 2 + 1] = 80;
            txt_ref[y * w + x] = 0.9;
            bub_ref[y * w + x] = 0.9;
        }
    }

    var needed: usize = 0;
    try std.testing.expectEqual(err_mod.k_ra8_ok, abi.ra8_translate_det_work_bytes(w, 8, &needed));
    var work: [4096]u8 = undefined;
    try std.testing.expect(needed <= work.len);

    var out_lines: [4]abi.DetLine = @splat(std.mem.zeroes(abi.DetLine));
    var out_bubbles: [4]abi.DetBubble = @splat(std.mem.zeroes(abi.DetBubble));
    var out_sum: abi.DetSummary = std.mem.zeroes(abi.DetSummary);
    out_sum.line_count = 777;

    const cfg_i8: abi.DetCfg = .{
        .map_width = w,
        .map_height = h,
        .row_stride = row_stride,
        .elem_stride = 2,
        .page_width = 120,
        .page_height = 100,
        .box_pad_pct = 10,
        .flags = 0,
        .max_components = 8,
    };

    // Null pointer & unmatched optional bubble_len errors must preserve out_sum!
    try std.testing.expectEqual(
        err_mod.k_ra8_err_null_ptr,
        abi.ra8_translate_det_post_i8(&nhwc_i8, nhwc_i8.len, 0.005, -100, null, 0, 0.005, -100, &cfg_i8, &work, needed, &out_lines, 4, &out_bubbles, 4, null),
    );
    try std.testing.expectEqual(
        err_mod.k_ra8_err_invalid_size,
        abi.ra8_translate_det_post_i8(&nhwc_i8, nhwc_i8.len, 0.005, -100, null, 5, 0.005, -100, &cfg_i8, &work, needed, &out_lines, 4, &out_bubbles, 4, &out_sum),
    );
    try std.testing.expectEqual(@as(u32, 777), out_sum.line_count);

    // Valid interleaved i8 post-processing
    const bub_slice_ptr: [*]const i8 = @ptrCast(nhwc_i8[1..].ptr);
    try std.testing.expectEqual(
        err_mod.k_ra8_ok,
        abi.ra8_translate_det_post_i8(&nhwc_i8, nhwc_i8.len, 0.005, -100, bub_slice_ptr, nhwc_i8.len - 1, 0.005, -100, &cfg_i8, &work, needed, &out_lines, 4, &out_bubbles, 4, &out_sum),
    );
    try std.testing.expectEqual(@as(u32, 1), out_sum.line_count);
    try std.testing.expectEqual(@as(u32, 1), out_sum.bubble_count);
    try std.testing.expectEqual(@as(u16, 9000), out_lines[0].confidence_bps);

    // Cross-check against packed f32 reference
    var f_lines: [4]abi.DetLine = @splat(std.mem.zeroes(abi.DetLine));
    var f_bubbles: [4]abi.DetBubble = @splat(std.mem.zeroes(abi.DetBubble));
    var f_sum: abi.DetSummary = undefined;
    var cfg_f32 = cfg_i8;
    cfg_f32.row_stride = 0;
    cfg_f32.elem_stride = 0;
    try std.testing.expectEqual(
        err_mod.k_ra8_ok,
        abi.ra8_translate_det_post_f32(&txt_ref, txt_ref.len, &bub_ref, bub_ref.len, &cfg_f32, &work, needed, &f_lines, 4, &f_bubbles, 4, &f_sum),
    );
    try std.testing.expectEqual(out_sum.line_count, f_sum.line_count);
    try std.testing.expectEqual(out_lines[0].x, f_lines[0].x);
    try std.testing.expectEqual(out_lines[0].w, f_lines[0].w);
    try std.testing.expectEqual(out_bubbles[0].confidence_bps, f_bubbles[0].confidence_bps);

    // NaN in float map fails with invalid_arg and preserves caller out_sum.
    txt_ref[0] = std.math.nan(f32);
    f_sum.bubble_count = 888;
    try std.testing.expectEqual(
        err_mod.k_ra8_err_invalid_arg,
        abi.ra8_translate_det_post_f32(&txt_ref, txt_ref.len, null, 0, &cfg_f32, &work, needed, &f_lines, 4, &f_bubbles, 4, &f_sum),
    );
    try std.testing.expectEqual(@as(u32, 888), f_sum.bubble_count);
}
