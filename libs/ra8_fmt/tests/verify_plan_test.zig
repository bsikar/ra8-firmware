//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Unit tests for `libs/ra8_fmt/src/internal/verify_plan.zig`.

const std = @import("std");
const c_abi = @import("c_abi");
const verify_plan = @import("verify_plan");
const mocks = @import("mocks");

fn pngHeader(color_type: u8, with_trns: bool) [64]u8 {
    var buf: [64]u8 = @splat(0);
    @memcpy(buf[0..8], "\x89PNG\r\n\x1a\n");
    // IHDR chunk length=13, "IHDR", width=8, height=4, depth=8, color_type
    buf[11] = 13;
    @memcpy(buf[12..16], "IHDR");
    buf[19] = 8;
    buf[23] = 4;
    buf[24] = 8;
    buf[25] = color_type;
    if (color_type == 3) {
        var off: usize = 33;
        if (with_trns) {
            @memcpy(buf[off + 4 ..][0..4], "tRNS");
            off += 12;
        }
        @memcpy(buf[off + 4 ..][0..4], "IDAT");
    }
    return buf;
}

test "jofVerifyRequirements maps PNG grayscale, RGB, palette tRNS, and RGBA channels" {
    const deps = comptime mocks.defaultStubDeps();
    const cases = [_]struct { color: u8, trns: bool, bpp: u8 }{
        .{ .color = 0, .trns = false, .bpp = 1 },
        .{ .color = 2, .trns = false, .bpp = 3 },
        .{ .color = 3, .trns = false, .bpp = 3 },
        .{ .color = 3, .trns = true, .bpp = 4 },
        .{ .color = 4, .trns = false, .bpp = 4 },
        .{ .color = 6, .trns = false, .bpp = 4 },
    };

    for (cases) |tc| {
        const bytes = pngHeader(tc.color, tc.trns);
        var src_state = mocks.MemSource{ .data = &bytes };
        const src = src_state.asSource(false);
        var req: c_abi.JofVerifyRequirements = undefined;

        try std.testing.expectEqual(c_abi.k_ra8_ok, verify_plan.jofVerifyRequirements(deps, &src, &req));
        try std.testing.expectEqual(tc.bpp, req.bpp);
        try std.testing.expectEqual(@as(u32, 8) * tc.bpp, req.row_bytes);
        try std.testing.expectEqual(req.row_bytes * 4, req.band_tile_bytes);
        try std.testing.expect(req.scratch_bytes >= req.band_tile_bytes + 256);
    }
}

test "jofVerifyRequirements detects JPEG monochrome, RGB, and unsupported CMYK" {
    const deps = comptime mocks.defaultStubDeps();
    const jpeg_gray = [_]u8{
        0xFF, 0xD8,
        0xFF, 0xC0,
        0x00, 0x0B,
        0x08, 0x00,
        0x04, 0x00,
        0x08, 0x01,
        0x01, 0x11,
        0x00, 0x00,
        0x00, 0x00,
        0x00, 0x00,
        0x00, 0x00,
        0x00, 0x00,
        0x00, 0x00,
        0x00,
    };
    var g_state = mocks.MemSource{ .data = &jpeg_gray };
    const g_src = g_state.asSource(false);
    var req: c_abi.JofVerifyRequirements = undefined;
    try std.testing.expectEqual(c_abi.k_ra8_ok, verify_plan.jofVerifyRequirements(deps, &g_src, &req));
    try std.testing.expectEqual(@as(u8, 1), req.bpp);

    const jpeg_cmyk = [_]u8{
        0xFF, 0xD8,
        0xFF, 0xC0,
        0x00, 0x0B,
        0x08, 0x00,
        0x04, 0x00,
        0x08, 0x04,
        0x01, 0x11,
        0x00, 0x00,
        0x00, 0x00,
        0x00, 0x00,
        0x00, 0x00,
        0x00, 0x00,
        0x00, 0x00,
        0x00,
    };
    var c_state = mocks.MemSource{ .data = &jpeg_cmyk };
    const c_src = c_state.asSource(false);
    try std.testing.expectEqual(c_abi.k_ra8_err_not_supported, verify_plan.jofVerifyRequirements(deps, &c_src, &req));
}

test "jofVerifyRequirements sets bpp=4 for WebP and runs source validator" {
    const deps = comptime mocks.defaultStubDeps();
    var webp: [28]u8 = @splat(0);
    @memcpy(webp[0..12], "RIFF\x18\x00\x00\x00WEBP");
    var ok_state = mocks.MemSource{ .data = &webp };
    const ok_src = ok_state.asSource(true);
    var req: c_abi.JofVerifyRequirements = undefined;
    try std.testing.expectEqual(c_abi.k_ra8_ok, verify_plan.jofVerifyRequirements(deps, &ok_src, &req));
    try std.testing.expectEqual(@as(u8, 4), req.bpp);
    try std.testing.expectEqual(@as(u32, 1), ok_state.validate_calls);

    var bad_state = mocks.MemSource{ .data = &webp, .validate_rc = c_abi.k_ra8_err_conflict };
    const bad_src = bad_state.asSource(true);
    try std.testing.expectEqual(@as(c_abi.Ra8Err, 0x408), verify_plan.jofVerifyRequirements(deps, &bad_src, &req));
}
