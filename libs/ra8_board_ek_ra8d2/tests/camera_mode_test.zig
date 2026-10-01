//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Every field of both capture policies, read back. This layer is pure, so
//! there is nothing to fake and nothing it can do that these assertions miss.

const std = @import("std");
const camera_mode = @import("camera_mode");

const Err = camera_mode.Err;
const Mode = camera_mode.Mode;
const Sensor = camera_mode.Sensor;

fn getOk(mode: u8, capacity: u32) camera_mode.BoardConfig {
    var cfg: camera_mode.BoardConfig = undefined;
    const err = camera_mode.get(mode, capacity, &cfg);
    std.debug.assert(err == Err.ok);
    return cfg;
}

test "a mode at or past the count is refused" {
    var cfg: camera_mode.BoardConfig = undefined;
    try std.testing.expectEqual(Err.invalid_arg, camera_mode.get(Mode.count, 0xDEAD, &cfg));
    try std.testing.expectEqual(Err.invalid_arg, camera_mode.get(0xFF, 0xDEAD, &cfg));
}

test "a zero capacity is refused for either mode" {
    var cfg: camera_mode.BoardConfig = undefined;
    try std.testing.expectEqual(Err.invalid_size, camera_mode.get(Mode.vga_uyvy, 0, &cfg));
    try std.testing.expectEqual(Err.invalid_size, camera_mode.get(Mode.vga_jpeg, 0, &cfg));
}

test "UYVY refuses a buffer under one packed frame and accepts exactly one" {
    var cfg: camera_mode.BoardConfig = undefined;
    try std.testing.expectEqual(614_400, camera_mode.packed_frame_bytes);
    try std.testing.expectEqual(
        Err.invalid_size,
        camera_mode.get(Mode.vga_uyvy, camera_mode.packed_frame_bytes - 1, &cfg),
    );
    try std.testing.expectEqual(
        Err.ok,
        camera_mode.get(Mode.vga_uyvy, camera_mode.packed_frame_bytes, &cfg),
    );
}

test "JPEG takes any non-zero capacity, however small" {
    var cfg: camera_mode.BoardConfig = undefined;
    try std.testing.expectEqual(Err.ok, camera_mode.get(Mode.vga_jpeg, 1, &cfg));
    try std.testing.expectEqual(1, cfg.frame_bytes_max);
    try std.testing.expectEqual(1, cfg.ceu.image_area_size);
}

test "the out parameter is untouched when the call is refused" {
    var cfg: camera_mode.BoardConfig = undefined;
    cfg.frame_bytes_max = 0xA5A5;
    _ = camera_mode.get(Mode.count, 0xDEAD, &cfg);
    try std.testing.expectEqual(0xA5A5, cfg.frame_bytes_max);
    _ = camera_mode.get(Mode.vga_uyvy, 0, &cfg);
    try std.testing.expectEqual(0xA5A5, cfg.frame_bytes_max);
}

test "UYVY descriptor" {
    const cfg = getOk(Mode.vga_uyvy, 0xDEAD_BEEF);
    const ceu = cfg.ceu;

    try std.testing.expectEqual(640, ceu.width_px);
    try std.testing.expectEqual(480, ceu.height_px);
    try std.testing.expectEqual(0, ceu.x_start_px);
    try std.testing.expectEqual(0, ceu.y_start_px);
    try std.testing.expectEqual(1280, ceu.x_capture_px);
    try std.testing.expectEqual(480, ceu.y_capture_lines);
    try std.testing.expectEqual(1280, ceu.dst_stride);
    try std.testing.expectEqual(0, ceu.frame_drop);
    try std.testing.expectEqual(2, ceu.bytes_per_pixel);
    try std.testing.expectEqual(0, ceu.interrupts);

    try std.testing.expectEqual(1, ceu.capture_format);
    try std.testing.expectEqual(0, ceu.capture_mode);
    try std.testing.expectEqual(0, ceu.data_bus);
    try std.testing.expectEqual(0, ceu.hsync_polarity);
    try std.testing.expectEqual(0, ceu.vsync_polarity);
    try std.testing.expectEqual(0, ceu.field_polarity);
    try std.testing.expectEqual(0, ceu.input_order);
    try std.testing.expectEqual(1, ceu.output_format);
    try std.testing.expectEqual(0, ceu.burst_mode);
    try std.testing.expectEqual(0, ceu.first_field);

    try std.testing.expectEqual(0, ceu.edge.data);
    try std.testing.expectEqual(0, ceu.edge.hsync);
    try std.testing.expectEqual(0, ceu.edge.vsync);
    try std.testing.expectEqual(0, ceu.edge.field);

    try std.testing.expectEqual(false, ceu.byte_swap.swap_8_bit);
    try std.testing.expectEqual(true, ceu.byte_swap.swap_16_bit);
    try std.testing.expectEqual(true, ceu.byte_swap.swap_32_bit);

    try std.testing.expectEqual(0, ceu.scale.h_mantissa);
    try std.testing.expectEqual(0, ceu.scale.h_fraction);
    try std.testing.expectEqual(0, ceu.scale.v_mantissa);
    try std.testing.expectEqual(0, ceu.scale.v_fraction);
    try std.testing.expectEqual(640, ceu.scale.h_output_clip);
    try std.testing.expectEqual(480, ceu.scale.v_output_clip);

    try std.testing.expectEqual(false, ceu.interlace);
    try std.testing.expectEqual(false, ceu.one_field_only);
    try std.testing.expectEqual(false, ceu.bundle_write);
    try std.testing.expectEqual(false, ceu.low_pass_filter);
    try std.testing.expectEqual(0, ceu.image_area_size);

    try std.testing.expectEqual(0xDEAD_BEEF, cfg.frame_bytes_max);
    try std.testing.expectEqual(1280, cfg.stride_bytes);
    try std.testing.expectEqual(5, cfg.poll_interval_ms);
    try std.testing.expectEqual(800, cfg.poll_attempts);
    try std.testing.expectEqual(24_000_000, cfg.xclk_hz);
    try std.testing.expectEqual(100, cfg.settle_ms);
    try std.testing.expectEqual(640, cfg.width_px);
    try std.testing.expectEqual(480, cfg.height_px);
}

test "JPEG descriptor" {
    const cfg = getOk(Mode.vga_jpeg, 0x4000);
    const ceu = cfg.ceu;

    try std.testing.expectEqual(640, ceu.width_px);
    try std.testing.expectEqual(480, ceu.height_px);
    try std.testing.expectEqual(640, ceu.x_capture_px);
    try std.testing.expectEqual(480, ceu.y_capture_lines);
    try std.testing.expectEqual(640, ceu.dst_stride);
    try std.testing.expectEqual(1, ceu.bytes_per_pixel);

    try std.testing.expectEqual(2, ceu.capture_format);
    try std.testing.expectEqual(0, ceu.capture_mode);
    try std.testing.expectEqual(3, ceu.burst_mode);
    try std.testing.expectEqual(1, ceu.output_format);

    try std.testing.expectEqual(true, ceu.byte_swap.swap_8_bit);
    try std.testing.expectEqual(true, ceu.byte_swap.swap_16_bit);
    try std.testing.expectEqual(true, ceu.byte_swap.swap_32_bit);

    try std.testing.expectEqual(0, ceu.scale.h_output_clip);
    try std.testing.expectEqual(0, ceu.scale.v_output_clip);
    try std.testing.expectEqual(0x4000, ceu.image_area_size);

    try std.testing.expectEqual(0x4000, cfg.frame_bytes_max);
    try std.testing.expectEqual(0, cfg.stride_bytes);
    try std.testing.expectEqual(2, cfg.poll_interval_ms);
    try std.testing.expectEqual(2000, cfg.poll_attempts);
    try std.testing.expectEqual(24_000_000, cfg.xclk_hz);
    try std.testing.expectEqual(100, cfg.settle_ms);
}

test "the two modes differ exactly where the fetch style differs" {
    const uyvy = getOk(Mode.vga_uyvy, 0x10_0000);
    const jpeg = getOk(Mode.vga_jpeg, 0x10_0000);

    try std.testing.expectEqual(uyvy.ceu.width_px, jpeg.ceu.width_px);
    try std.testing.expectEqual(uyvy.ceu.height_px, jpeg.ceu.height_px);
    try std.testing.expectEqual(uyvy.xclk_hz, jpeg.xclk_hz);
    try std.testing.expectEqual(uyvy.settle_ms, jpeg.settle_ms);

    try std.testing.expect(uyvy.ceu.capture_format != jpeg.ceu.capture_format);
    try std.testing.expect(uyvy.ceu.burst_mode != jpeg.ceu.burst_mode);
    try std.testing.expect(uyvy.ceu.bytes_per_pixel != jpeg.ceu.bytes_per_pixel);
    try std.testing.expect(uyvy.stride_bytes != jpeg.stride_bytes);
    try std.testing.expect(uyvy.ceu.byte_swap.swap_8_bit != jpeg.ceu.byte_swap.swap_8_bit);
}

test "the mirrored descriptor matches the C layout the HAL expects" {
    const Config = camera_mode.Config;
    try std.testing.expectEqual(4, @alignOf(Config));
    try std.testing.expectEqual(0, @offsetOf(Config, "width_px"));
    try std.testing.expectEqual(14, @offsetOf(Config, "frame_drop"));
    try std.testing.expectEqual(16, @offsetOf(Config, "interrupts"));
    try std.testing.expectEqual(20, @offsetOf(Config, "capture_format"));
    try std.testing.expectEqual(30, @offsetOf(Config, "edge"));
    try std.testing.expectEqual(34, @offsetOf(Config, "byte_swap"));
    try std.testing.expectEqual(38, @offsetOf(Config, "scale"));
    try std.testing.expectEqual(50, @offsetOf(Config, "interlace"));
    try std.testing.expectEqual(56, @offsetOf(Config, "image_area_size"));
    try std.testing.expectEqual(60, @sizeOf(Config));

    const Board = camera_mode.BoardConfig;
    try std.testing.expectEqual(0, @offsetOf(Board, "ceu"));
    try std.testing.expectEqual(60, @offsetOf(Board, "frame_bytes_max"));
    try std.testing.expectEqual(84, @offsetOf(Board, "width_px"));
    try std.testing.expectEqual(88, @sizeOf(Board));
}
