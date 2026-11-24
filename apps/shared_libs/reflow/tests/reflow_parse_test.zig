// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

//! Root host unit test harness for `apps/shared_libs/reflow`.

const std = @import("std");
const abi = @import("reflow");

comptime {
    _ = &@import("ra8_stbtt_guard").ra8_stbtt_sfnt_dir_in_bounds;
    _ = @import("reflow_image_test_seam.zig");
    _ = @import("reflow_render_test_seam.zig");
    _ = @import("ra8_img_imgdec_test.zig");
    _ = @import("reflow_cache_test.zig");
    _ = @import("reflow_css_test.zig");
    _ = @import("reflow_css_cascade_test.zig");
    _ = @import("reflow_css_rules_test.zig");
    _ = @import("reflow_image_test.zig");
    _ = @import("reflow_render_face_test.zig");
    _ = @import("reflow_render_test.zig");
    _ = @import("reflow_svg_test.zig");
    _ = @import("reflow_svg_doc_test.zig");
    _ = @import("reflow_svg_path_test.zig");
    _ = @import("reflow_svg_shape_test.zig");
    _ = @import("reflow_svg_xform_test.zig");
    _ = @import("reflow_bidi_test.zig");
    _ = @import("reflow_shaping_test.zig");
}

const ReflowEngine = abi.ReflowEngine;

const k_ra8_tok_block_start: u8 = 0;
const k_ra8_tok_block_end: u8 = 1;
const k_ra8_tok_text: u8 = 2;
const k_ra8_tag_p: u8 = 1;

const k_ra8_err_invalid_size: c_int = 0x105;
const k_ra8_err_not_initialized: c_int = 0x10F;
const k_ra8_err_null_ptr: c_int = 0x504;
const k_ra8_err_validation_failed: c_int = 0x501;

test "mirror layout sanity" {
    comptime {
        std.debug.assert(@offsetOf(ReflowEngine, "in_use") == @sizeOf(ReflowEngine) - 8);
        std.debug.assert(@offsetOf(ReflowEngine, "token_count") < @offsetOf(ReflowEngine, "text_pool"));
        std.debug.assert(@offsetOf(ReflowEngine, "text_pool_used") < @offsetOf(ReflowEngine, "link_targets"));
        std.debug.assert(@offsetOf(ReflowEngine, "link_target_count") < @offsetOf(ReflowEngine, "link_rects"));
    }
}

test "argument gates" {
    const engine = try std.heap.page_allocator.create(ReflowEngine);
    defer std.heap.page_allocator.destroy(engine);
    engine.* = std.mem.zeroes(ReflowEngine);

    try std.testing.expectEqual(k_ra8_err_null_ptr, abi.reflow_parse_xhtml(null, "x", 1));
    try std.testing.expectEqual(k_ra8_err_null_ptr, abi.reflow_parse_xhtml(engine, null, 1));
    try std.testing.expectEqual(k_ra8_err_not_initialized, abi.reflow_parse_xhtml(engine, "x", 1));
    engine.in_use = 1;
    try std.testing.expectEqual(k_ra8_err_invalid_size, abi.reflow_parse_xhtml(engine, "x", 0));

    try std.testing.expectEqual(k_ra8_err_null_ptr, abi.tokenize.priv_reflow_xml_walk(null, "x", 1));
    try std.testing.expectEqual(k_ra8_err_null_ptr, abi.tokenize.priv_reflow_xml_walk(engine, null, 1));
    try std.testing.expectEqual(k_ra8_err_invalid_size, abi.tokenize.priv_reflow_xml_walk(engine, "x", 0));
}

test "pool reset then real walk" {
    const engine = try std.heap.page_allocator.create(ReflowEngine);
    defer std.heap.page_allocator.destroy(engine);
    engine.* = std.mem.zeroes(ReflowEngine);
    engine.in_use = 1;
    engine.token_count = 5;
    engine.text_pool_used = 9;
    engine.link_target_count = 3;

    try std.testing.expectEqual(@as(c_int, 0), abi.reflow_parse_xhtml(engine, "<p>hi</p>", 9));
    try std.testing.expectEqual(@as(u32, 0), engine.text_pool_used - 2);
    try std.testing.expectEqualStrings("hi", engine.text_pool[0..2]);
    try std.testing.expectEqual(@as(u32, 3), engine.token_count);
    try std.testing.expectEqual(k_ra8_tok_block_start, engine.tokens[0].kind);
    try std.testing.expectEqual(k_ra8_tag_p, engine.tokens[0].tag);
    try std.testing.expectEqual(k_ra8_tok_text, engine.tokens[1].kind);
    try std.testing.expectEqual(k_ra8_tok_block_end, engine.tokens[2].kind);
    try std.testing.expectEqual(@as(u32, 0), engine.link_target_count);
}

test "text outside any element is dropped" {
    const engine = try std.heap.page_allocator.create(ReflowEngine);
    defer std.heap.page_allocator.destroy(engine);
    engine.* = std.mem.zeroes(ReflowEngine);
    engine.in_use = 1;

    try std.testing.expectEqual(k_ra8_err_validation_failed, abi.reflow_parse_xhtml(engine, "just text", 9));
    try std.testing.expectEqual(k_ra8_err_validation_failed, abi.reflow_parse_xhtml(engine, "<![CDATA[orphan]]>", 18));
    try std.testing.expectEqual(@as(u32, 0), engine.token_count);
    try std.testing.expectEqual(@as(u32, 0), engine.text_pool_used);
}

test "unbalanced element fails validation" {
    const engine = try std.heap.page_allocator.create(ReflowEngine);
    defer std.heap.page_allocator.destroy(engine);
    engine.* = std.mem.zeroes(ReflowEngine);
    engine.in_use = 1;

    try std.testing.expectEqual(k_ra8_err_validation_failed, abi.reflow_parse_xhtml(engine, "<p>hi", 5));
}
