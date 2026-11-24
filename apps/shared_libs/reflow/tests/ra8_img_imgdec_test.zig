// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

//! Unit tests for the `ra8_imgdec` stb_image adapter (`src/ra8_img_imgdec.zig`).

const std = @import("std");
const imgdec = @import("reflow").imgdec;

const testing = std.testing;
const Ra8ImgdecReq = imgdec.Ra8ImgdecReq;
const Ra8ImgdecCaps = imgdec.Ra8ImgdecCaps;
const Ra8Imgdec = imgdec.Ra8Imgdec;
const Ra8ImgdecIface = imgdec.Ra8ImgdecIface;

test "destination check honours a padded stride" {
    const req: Ra8ImgdecReq = .{
        .bytes = null,
        .byte_count = 1,
        .arena = null,
        .dst = null,
        .dst_bytes = 24,
        .dst_stride = 10,
        .format = 0,
        .want = 0,
    };
    var need: u32 = 0;
    try testing.expectEqual(imgdec.k_ra8_ok, imgdec.internalDstOk(&req, 2, 8, &need));
    try testing.expectEqual(@as(u32, 18), need);
}

test "destination check refuses a stride under one packed row" {
    const req: Ra8ImgdecReq = .{
        .bytes = null,
        .byte_count = 1,
        .arena = null,
        .dst = null,
        .dst_bytes = 64,
        .dst_stride = 7,
        .format = 0,
        .want = 0,
    };
    var need: u32 = 0;
    try testing.expectEqual(imgdec.k_ra8_err_invalid_size, imgdec.internalDstOk(&req, 2, 8, &need));
}

test "destination check refuses a destination under the exact span" {
    const req: Ra8ImgdecReq = .{
        .bytes = null,
        .byte_count = 1,
        .arena = null,
        .dst = null,
        .dst_bytes = 17,
        .dst_stride = 10,
        .format = 0,
        .want = 0,
    };
    var need: u32 = 0;
    try testing.expectEqual(imgdec.k_ra8_err_invalid_size, imgdec.internalDstOk(&req, 2, 8, &need));
}

test "destination check accepts an exactly-sized destination" {
    const req: Ra8ImgdecReq = .{
        .bytes = null,
        .byte_count = 1,
        .arena = null,
        .dst = null,
        .dst_bytes = 18,
        .dst_stride = 0,
        .format = 0,
        .want = 0,
    };
    var need: u32 = 0;
    try testing.expectEqual(imgdec.k_ra8_ok, imgdec.internalDstOk(&req, 2, 9, &need));
    try testing.expectEqual(@as(u32, 18), need);
}

test "caps advertise the stb residue" {
    var caps: Ra8ImgdecCaps = undefined;
    try testing.expectEqual(imgdec.k_ra8_ok, imgdec.internalCaps(null, &caps));
    try testing.expectEqual(imgdec.k_ra8_imgdec_format_png | imgdec.k_ra8_imgdec_format_gif | imgdec.k_ra8_imgdec_format_bmp, caps.formats);
    try testing.expectEqual(imgdec.k_ra8_imgdec_pixel_grey8 | imgdec.k_ra8_imgdec_pixel_rgb888 | imgdec.k_ra8_imgdec_pixel_rgba8888, caps.pixels);
    try testing.expectEqual(@as(u32, 0), caps.scratch_bytes);
    try testing.expectEqual(@as(u32, 0), caps.scratch_align);
    try testing.expectEqual(@as(u32, 8192), caps.dim_max);
    try testing.expectEqual(false, caps.streams);
}

test "mirror layout sanity" {
    comptime {
        std.debug.assert(@offsetOf(Ra8ImgdecReq, "byte_count") == @sizeOf(?[*]const u8));
        std.debug.assert(@offsetOf(Ra8Imgdec, "ctx") == @sizeOf(?*const Ra8ImgdecIface));
    }
}
