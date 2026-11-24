//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Exported C ABI boundary and failure-precedence tests for `libs/ra8_translate`.

const std = @import("std");
const abi = @import("ra8_translate_abi");
const err_mod = @import("err");

test "C ABI exports enforce null-first precedence and leave outputs untouched on error" {
    var sentinel_hdr: abi.SidecarHeader = std.mem.zeroes(abi.SidecarHeader);
    sentinel_hdr.total_bytes = 0xDEAD_BEEF;

    try std.testing.expectEqual(err_mod.k_ra8_err_null_ptr, abi.ra8_translate_sidecar_open(null, 0, null));
    try std.testing.expectEqual(err_mod.k_ra8_err_null_ptr, abi.ra8_translate_sidecar_open(null, 10, &sentinel_hdr));
    try std.testing.expectEqual(@as(u32, 0xDEAD_BEEF), sentinel_hdr.total_bytes);

    try std.testing.expectEqual(
        err_mod.k_ra8_err_invalid_size,
        abi.ra8_translate_sidecar_open(null, 0, &sentinel_hdr),
    );
    try std.testing.expectEqual(@as(u32, 0xDEAD_BEEF), sentinel_hdr.total_bytes);

    var matches = true;
    try std.testing.expectEqual(
        err_mod.k_ra8_err_null_ptr,
        abi.ra8_translate_sidecar_key_matches(null, null, &matches),
    );
    try std.testing.expect(matches);
}

test "C ABI flat-table write and read exports round-trip cleanly" {
    var key = abi.SidecarKey{
        .chapter_hash = @splat(0x42),
        .detector_model_version = 10,
        .ocr_model_version = 20,
        .nmt_model_version = 30,
        .glossary_revision = 5,
        .source_lang = 1,
        .reserved = .{ 0, 0, 0 },
    };
    const pages = [_]abi.PageEntry{
        .{ .page_index = 0, .bubble_count = 1, .first_bubble = 0, .page_width = 600, .page_height = 800 },
    };
    const breaks = [_]u16{ 2, 5 };
    const pool = "okhello";
    const bubbles = [_]abi.BubbleRecord{
        .{
            .x = 10,
            .y = 20,
            .w = 100,
            .h = 40,
            .shape = 2,
            .text_orientation = 0,
            .corner_radius = 6,
            .reading_order = 0,
            .ocr_confidence_bps = 9100,
            .font_size_px = 16,
            .line_count = 2,
            .first_line_break = 0,
            .source_text_off = 0,
            .source_text_len = 2,
            .english_text_off = 2,
            .english_text_len = 5,
        },
    };

    var needed: usize = 0;
    try std.testing.expectEqual(
        err_mod.k_ra8_ok,
        abi.ra8_translate_sidecar_required_bytes(1, 1, 2, pool.len, &needed),
    );
    try std.testing.expectEqual(@as(usize, 96 + 12 + 44 + 4 + 7), needed);

    var buf: [256]u8 = undefined;
    var written: usize = 0;
    try std.testing.expectEqual(
        err_mod.k_ra8_ok,
        abi.ra8_translate_sidecar_write(
            &key,
            &pages,
            1,
            &bubbles,
            1,
            &breaks,
            2,
            pool.ptr,
            pool.len,
            &buf,
            buf.len,
            &written,
        ),
    );
    try std.testing.expectEqual(needed, written);

    var hdr: abi.SidecarHeader = undefined;
    try std.testing.expectEqual(err_mod.k_ra8_ok, abi.ra8_translate_sidecar_open(&buf, written, &hdr));

    var matched = false;
    try std.testing.expectEqual(err_mod.k_ra8_ok, abi.ra8_translate_sidecar_key_matches(&hdr, &key, &matched));
    try std.testing.expect(matched);

    var p0: abi.PageEntry = undefined;
    try std.testing.expectEqual(err_mod.k_ra8_ok, abi.ra8_translate_sidecar_page_at(&buf, written, 0, &p0));
    try std.testing.expectEqual(@as(u16, 600), p0.page_width);

    var b0: abi.BubbleRecord = undefined;
    try std.testing.expectEqual(err_mod.k_ra8_ok, abi.ra8_translate_sidecar_bubble_at(&buf, written, 0, &b0));
    try std.testing.expectEqual(@as(u16, 9100), b0.ocr_confidence_bps);

    var src_ptr: ?[*]const u8 = null;
    var src_len: u32 = 0;
    try std.testing.expectEqual(
        err_mod.k_ra8_ok,
        abi.ra8_translate_sidecar_source_text(&buf, written, 0, &src_ptr, &src_len),
    );
    try std.testing.expectEqualStrings("ok", src_ptr.?[0..src_len]);

    var eng_ptr: ?[*]const u8 = null;
    var eng_len: u32 = 0;
    try std.testing.expectEqual(
        err_mod.k_ra8_ok,
        abi.ra8_translate_sidecar_english_text(&buf, written, 0, &eng_ptr, &eng_len),
    );
    try std.testing.expectEqualStrings("hello", eng_ptr.?[0..eng_len]);

    var out_breaks = [_]u16{ 0xFFFF, 0xFFFF, 0xFFFF };
    var out_cnt: u16 = 99;
    try std.testing.expectEqual(
        err_mod.k_ra8_err_invalid_size,
        abi.ra8_translate_sidecar_line_breaks(&buf, written, 0, &out_breaks, 1, &out_cnt),
    );
    try std.testing.expectEqual(@as(u16, 0xFFFF), out_breaks[0]);
    try std.testing.expectEqual(@as(u16, 99), out_cnt);

    try std.testing.expectEqual(
        err_mod.k_ra8_ok,
        abi.ra8_translate_sidecar_line_breaks(&buf, written, 0, &out_breaks, 3, &out_cnt),
    );
    try std.testing.expectEqual(@as(u16, 2), out_cnt);
    try std.testing.expectEqualSlices(u16, &breaks, out_breaks[0..out_cnt]);
}
