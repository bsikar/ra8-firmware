//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C23 ABI adapter exports for `libs/ra8_translate` DB detector post-processing.

const std = @import("std");
const det = @import("detector_post");
const err_mod = @import("err");

pub const DetLine = det.DetLine;
pub const DetBubble = det.DetBubble;
pub const DetCfg = det.DetCfg;
pub const DetSummary = det.DetSummary;

comptime {
    const assert = std.debug.assert;
    assert(@sizeOf(det.ReadingOrder) == 1);
    assert(@sizeOf(DetLine) == 16);
    assert(@alignOf(DetLine) == 2);
    assert(@offsetOf(DetLine, "x") == 0);
    assert(@offsetOf(DetLine, "y") == 2);
    assert(@offsetOf(DetLine, "w") == 4);
    assert(@offsetOf(DetLine, "h") == 6);
    assert(@offsetOf(DetLine, "bubble_index") == 8);
    assert(@offsetOf(DetLine, "confidence_bps") == 10);
    assert(@offsetOf(DetLine, "text_orientation") == 12);
    assert(@offsetOf(DetLine, "flags") == 13);
    assert(@offsetOf(DetLine, "order_in_bubble") == 14);

    assert(@sizeOf(DetBubble) == 20);
    assert(@alignOf(DetBubble) == 2);
    assert(@offsetOf(DetBubble, "x") == 0);
    assert(@offsetOf(DetBubble, "y") == 2);
    assert(@offsetOf(DetBubble, "w") == 4);
    assert(@offsetOf(DetBubble, "h") == 6);
    assert(@offsetOf(DetBubble, "reading_order") == 8);
    assert(@offsetOf(DetBubble, "confidence_bps") == 10);
    assert(@offsetOf(DetBubble, "first_line") == 12);
    assert(@offsetOf(DetBubble, "line_count") == 14);
    assert(@offsetOf(DetBubble, "shape") == 16);
    assert(@offsetOf(DetBubble, "text_orientation") == 17);
    assert(@offsetOf(DetBubble, "flags") == 18);
    assert(@offsetOf(DetBubble, "reserved") == 19);

    assert(@sizeOf(DetCfg) == 32);
    assert(@alignOf(DetCfg) == 2);
    assert(@offsetOf(DetCfg, "map_width") == 0);
    assert(@offsetOf(DetCfg, "page_width") == 8);
    assert(@offsetOf(DetCfg, "text_thresh_bps") == 12);
    assert(@offsetOf(DetCfg, "reading_order") == 26);
    assert(@offsetOf(DetCfg, "max_components") == 28);
    assert(@offsetOf(DetCfg, "reserved") == 30);

    assert(@sizeOf(DetSummary) == 16);
    assert(@alignOf(DetSummary) == 4);
    assert(@offsetOf(DetSummary, "raw_components") == 0);
    assert(@offsetOf(DetSummary, "dropped_furigana") == 4);
    assert(@offsetOf(DetSummary, "line_count") == 8);
    assert(@offsetOf(DetSummary, "bubble_count") == 12);
}

fn inputSlice(comptime T: type, ptr_opt: ?[*]const T, len: usize) ?[]const T {
    if (len == 0) return &.{};
    const ptr = ptr_opt orelse return null;
    return ptr[0..len];
}

fn optionalInputSlice(comptime T: type, ptr_opt: ?[*]const T, len: usize) Error_Or_OptSlice(T) {
    const ptr = ptr_opt orelse {
        if (len != 0) return .invalid_size;
        return .{ .none = {} };
    };
    if (len == 0) return .{ .some = &.{} };
    return .{ .some = ptr[0..len] };
}

fn Error_Or_OptSlice(comptime T: type) type {
    return union(enum) {
        none: void,
        some: []const T,
        invalid_size: void,
    };
}

/// C ABI export for `ra8_translate_det_work_bytes()`.
pub export fn ra8_translate_det_work_bytes(
    map_width: u16,
    max_components: u16,
    out_work_bytes_opt: ?*usize,
) callconv(.c) u16 {
    const out_work_bytes = out_work_bytes_opt orelse return err_mod.k_ra8_err_null_ptr;
    const needed = det.minWorkBytes(map_width, max_components) catch |e| return err_mod.toRa8Err(e);
    out_work_bytes.* = needed;
    return err_mod.k_ra8_ok;
}

/// C ABI export for `ra8_translate_det_post_i8()`.
pub export fn ra8_translate_det_post_i8(
    text_opt: ?[*]const i8,
    text_len: usize,
    text_scale: f32,
    text_zp: i8,
    bubble_opt: ?[*]const i8,
    bubble_len: usize,
    bubble_scale: f32,
    bubble_zp: i8,
    cfg_opt: ?*const DetCfg,
    work_opt: ?[*]u8,
    work_cap: usize,
    out_lines_opt: ?[*]DetLine,
    line_cap: usize,
    out_bubbles_opt: ?[*]DetBubble,
    bubble_cap: usize,
    out_summary_opt: ?*DetSummary,
) callconv(.c) u16 {
    const out_summary = out_summary_opt orelse return err_mod.k_ra8_err_null_ptr;
    const cfg = cfg_opt orelse return err_mod.k_ra8_err_null_ptr;
    const work_buf = work_opt orelse return err_mod.k_ra8_err_null_ptr;
    const out_lines = out_lines_opt orelse return err_mod.k_ra8_err_null_ptr;
    const out_bubbles = out_bubbles_opt orelse return err_mod.k_ra8_err_null_ptr;
    const text_map = inputSlice(i8, text_opt, text_len) orelse return err_mod.k_ra8_err_null_ptr;

    const bubble_slice: ?[]const i8 = switch (optionalInputSlice(i8, bubble_opt, bubble_len)) {
        .none => null,
        .some => |s| s,
        .invalid_size => return err_mod.k_ra8_err_invalid_size,
    };

    const summary = det.postProcessI8(
        text_map,
        text_scale,
        text_zp,
        bubble_slice,
        bubble_scale,
        bubble_zp,
        cfg.*,
        work_buf[0..work_cap],
        out_lines[0..line_cap],
        out_bubbles[0..bubble_cap],
    ) catch |e| return err_mod.toRa8Err(e);
    out_summary.* = summary;
    return err_mod.k_ra8_ok;
}

/// C ABI export for `ra8_translate_det_post_f32()`.
pub export fn ra8_translate_det_post_f32(
    text_opt: ?[*]const f32,
    text_len: usize,
    bubble_opt: ?[*]const f32,
    bubble_len: usize,
    cfg_opt: ?*const DetCfg,
    work_opt: ?[*]u8,
    work_cap: usize,
    out_lines_opt: ?[*]DetLine,
    line_cap: usize,
    out_bubbles_opt: ?[*]DetBubble,
    bubble_cap: usize,
    out_summary_opt: ?*DetSummary,
) callconv(.c) u16 {
    const out_summary = out_summary_opt orelse return err_mod.k_ra8_err_null_ptr;
    const cfg = cfg_opt orelse return err_mod.k_ra8_err_null_ptr;
    const work_buf = work_opt orelse return err_mod.k_ra8_err_null_ptr;
    const out_lines = out_lines_opt orelse return err_mod.k_ra8_err_null_ptr;
    const out_bubbles = out_bubbles_opt orelse return err_mod.k_ra8_err_null_ptr;
    const text_map = inputSlice(f32, text_opt, text_len) orelse return err_mod.k_ra8_err_null_ptr;

    const bubble_slice: ?[]const f32 = switch (optionalInputSlice(f32, bubble_opt, bubble_len)) {
        .none => null,
        .some => |s| s,
        .invalid_size => return err_mod.k_ra8_err_invalid_size,
    };

    const summary = det.postProcessF32(
        text_map,
        bubble_slice,
        cfg.*,
        work_buf[0..work_cap],
        out_lines[0..line_cap],
        out_bubbles[0..bubble_cap],
    ) catch |e| return err_mod.toRa8Err(e);
    out_summary.* = summary;
    return err_mod.k_ra8_ok;
}
