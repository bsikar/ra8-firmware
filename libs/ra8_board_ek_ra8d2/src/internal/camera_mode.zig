//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The DVP capture policy for the OV5640 on J35: which CEU descriptor each
//! supported mode wants, and how much buffer it costs. Pure, no hardware
//! access and no HAL calls, so every field here is checkable by reading it
//! back.

const ceu_types = @import("ceu_types.zig");
const vocab = @import("vocab.zig");

pub const Err = vocab.Err;
pub const Config = ceu_types.Config;
pub const BoardConfig = ceu_types.BoardConfig;

const Ceu = ceu_types.Ceu;

/// `ra8_board_camera_mode_t`.
pub const Mode = struct {
    pub const vga_uyvy: u8 = 0;
    pub const vga_jpeg: u8 = 1;
    /// Number of validated modes.
    pub const count: u8 = 2;
};

/// Geometry and timing shared by both modes.
pub const Sensor = struct {
    pub const vga_width: u16 = 640;
    pub const vga_height: u16 = 480;
    /// Packed UYVY sample pair.
    pub const uyvy_bytes_per_px: u8 = 2;
    /// Gated byte stream.
    pub const jpeg_bytes_per_px: u8 = 1;
    /// OV5640 input clock.
    pub const xclk_hz: u32 = 24_000_000;
    /// Delay after routing before the sensor is usable.
    pub const settle_ms: u32 = 100;
    pub const uyvy_poll_ms: u32 = 5;
    pub const uyvy_poll_tries: u32 = 800;
    pub const jpeg_poll_ms: u32 = 2;
    pub const jpeg_poll_tries: u32 = 2000;
};

/// Bytes one packed UYVY frame occupies.
pub const packed_frame_bytes: u32 =
    @as(u32, Sensor.vga_width) * @as(u32, Sensor.vga_height) * Sensor.uyvy_bytes_per_px;

const all_rising = ceu_types.EdgeInfo{
    .data = Ceu.edge_rising,
    .hsync = Ceu.edge_rising,
    .vsync = Ceu.edge_rising,
    .field = Ceu.edge_rising,
};

/// Packed UYVY: synchronous fetch of one 640x480 frame over the 8-bit DVP,
/// high-active syncs, everything latched on the rising edge, 32-byte bursts.
/// The word and dword swaps put memory in the UYVY order the software
/// converter reads. Clip-only scale block, no scale-down.
fn vgaUyvy(frame_bytes_max: u32) BoardConfig {
    const stride: u16 = Sensor.vga_width * Sensor.uyvy_bytes_per_px;
    return .{
        .ceu = .{
            .width_px = Sensor.vga_width,
            .height_px = Sensor.vga_height,
            .x_capture_px = stride,
            .y_capture_lines = Sensor.vga_height,
            .dst_stride = stride,
            .bytes_per_pixel = Sensor.uyvy_bytes_per_px,
            .capture_format = Ceu.fmt_data_synchronous,
            .capture_mode = Ceu.capture_single,
            .data_bus = Ceu.bus_8_bit,
            .hsync_polarity = Ceu.pol_high_active,
            .vsync_polarity = Ceu.pol_high_active,
            .field_polarity = Ceu.pol_high_active,
            .input_order = Ceu.input_cb0_y0_cr0_y1,
            .output_format = Ceu.output_ycbcr_422,
            .burst_mode = Ceu.burst_32,
            .first_field = Ceu.field_immediate,
            .edge = all_rising,
            .byte_swap = .{ .swap_8_bit = false, .swap_16_bit = true, .swap_32_bit = true },
            .scale = .{ .h_output_clip = Sensor.vga_width, .v_output_clip = Sensor.vga_height },
        },
        .frame_bytes_max = frame_bytes_max,
        .stride_bytes = stride,
        .poll_interval_ms = Sensor.uyvy_poll_ms,
        .poll_attempts = Sensor.uyvy_poll_tries,
        .xclk_hz = Sensor.xclk_hz,
        .settle_ms = Sensor.settle_ms,
        .width_px = Sensor.vga_width,
        .height_px = Sensor.vga_height,
    };
}

/// Sensor JPEG: a gated byte stream rather than a raster, so the frame has no
/// stride and the capacity becomes the data-enable firewall window. 256-byte
/// bursts, every swap on.
fn vgaJpeg(frame_bytes_max: u32) BoardConfig {
    return .{
        .ceu = .{
            .width_px = Sensor.vga_width,
            .height_px = Sensor.vga_height,
            .x_capture_px = Sensor.vga_width,
            .y_capture_lines = Sensor.vga_height,
            .dst_stride = Sensor.vga_width,
            .bytes_per_pixel = Sensor.jpeg_bytes_per_px,
            .capture_format = Ceu.fmt_data_enable,
            .capture_mode = Ceu.capture_single,
            .data_bus = Ceu.bus_8_bit,
            .hsync_polarity = Ceu.pol_high_active,
            .vsync_polarity = Ceu.pol_high_active,
            .field_polarity = Ceu.pol_high_active,
            .input_order = Ceu.input_cb0_y0_cr0_y1,
            .output_format = Ceu.output_ycbcr_422,
            .burst_mode = Ceu.burst_256,
            .first_field = Ceu.field_immediate,
            .edge = all_rising,
            .byte_swap = .{ .swap_8_bit = true, .swap_16_bit = true, .swap_32_bit = true },
            .image_area_size = frame_bytes_max,
        },
        .frame_bytes_max = frame_bytes_max,
        .stride_bytes = 0,
        .poll_interval_ms = Sensor.jpeg_poll_ms,
        .poll_attempts = Sensor.jpeg_poll_tries,
        .xclk_hz = Sensor.xclk_hz,
        .settle_ms = Sensor.settle_ms,
        .width_px = Sensor.vga_width,
        .height_px = Sensor.vga_height,
    };
}

/// Resolve a mode to its capture policy, refusing a buffer that cannot hold
/// what the mode produces. Only UYVY has a knowable frame size; a JPEG frame
/// is whatever the sensor emits, so any non-zero capacity is accepted and
/// becomes the firewall window.
pub fn get(mode: u8, frame_bytes_max: u32, out_config: *BoardConfig) u32 {
    if (mode >= Mode.count) return Err.invalid_arg;
    if (frame_bytes_max == 0) return Err.invalid_size;

    if (mode == Mode.vga_uyvy) {
        if (frame_bytes_max < packed_frame_bytes) return Err.invalid_size;
        out_config.* = vgaUyvy(frame_bytes_max);
        return Err.ok;
    }
    out_config.* = vgaJpeg(frame_bytes_max);
    return Err.ok;
}
