//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The Ring-3 CEU HAL seam: C mirrors of the register descriptor, the status
//! snapshot and the DMA address bundle from `libs/ra8_hal/inc/ra8_ceu_types.h`,
//! plus the six entry points the capture backend calls. Types only and no
//! exported symbols, so the backend that uses them stays one archive member.
//!
//! Every offset below was read off the C headers with `offsetof`, not inferred,
//! and is asserted at comptime on every target the archive is built for.

const std = @import("std");

/// `ra8_ceu_edge_info_t`: which VIO_CLK edge samples each input signal.
pub const EdgeInfo = extern struct {
    data: u8 = 0,
    hsync: u8 = 0,
    vsync: u8 = 0,
    field: u8 = 0,
};

/// `ra8_ceu_byte_swap_t`: CDOCR.COBS/COWS/COLS output swapping.
pub const ByteSwap = extern struct {
    swap_8_bit: bool = false,
    swap_16_bit: bool = false,
    swap_32_bit: bool = false,
};

/// `ra8_ceu_scale_t`: CFLCR scale factors with their CFSZR output clips.
pub const Scale = extern struct {
    h_mantissa: u16 = 0,
    h_fraction: u16 = 0,
    v_mantissa: u16 = 0,
    v_fraction: u16 = 0,
    h_output_clip: u16 = 0,
    v_output_clip: u16 = 0,
};

/// `ra8_ceu_config_t`: the whole register descriptor programmed at init. The
/// backend forwards it untouched and reads only `capture_format` and
/// `image_area_size` out of it.
pub const Config = extern struct {
    width_px: u16 = 0,
    height_px: u16 = 0,
    x_start_px: u16 = 0,
    y_start_px: u16 = 0,
    x_capture_px: u16 = 0,
    y_capture_lines: u16 = 0,
    dst_stride: u16 = 0,
    frame_drop: u8 = 0,
    bytes_per_pixel: u8 = 0,
    interrupts: u32 = 0,
    capture_format: u8 = 0,
    capture_mode: u8 = 0,
    data_bus: u8 = 0,
    hsync_polarity: u8 = 0,
    vsync_polarity: u8 = 0,
    field_polarity: u8 = 0,
    input_order: u8 = 0,
    output_format: u8 = 0,
    burst_mode: u8 = 0,
    first_field: u8 = 0,
    edge: EdgeInfo = .{},
    byte_swap: ByteSwap = .{},
    scale: Scale = .{},
    interlace: bool = false,
    one_field_only: bool = false,
    bundle_write: bool = false,
    low_pass_filter: bool = false,
    image_area_size: u32 = 0,
};

/// `ra8_ceu_status_t`: one observed snapshot of CETCR, CDSSR and CSTSR.
pub const Status = extern struct {
    events: u32 = 0,
    data_size: u32 = 0,
    capturing: bool = false,
    reset_in_flight: bool = false,
    active_plane: u8 = 0,
    top_field: bool = false,
};

/// `ra8_ceu_buffers_t`: the DMA destination addresses armed for one capture.
/// This backend fills `y_top` only; every other row stays null, exactly as the
/// C did with its designated initializer.
pub const Buffers = extern struct {
    y_top: ?[*]u8 = null,
    c_top: ?[*]u8 = null,
    y_bottom: ?[*]u8 = null,
    c_bottom: ?[*]u8 = null,
    y_top_2: ?[*]u8 = null,
    c_top_2: ?[*]u8 = null,
    y_bottom_2: ?[*]u8 = null,
    c_bottom_2: ?[*]u8 = null,
    bundle_size_bytes: u32 = 0,
};

const ptr_bytes = @sizeOf(usize);

comptime {
    std.debug.assert(@sizeOf(EdgeInfo) == 4);
    std.debug.assert(@sizeOf(ByteSwap) == 3);
    std.debug.assert(@sizeOf(Scale) == 12);

    std.debug.assert(@offsetOf(Config, "width_px") == 0);
    std.debug.assert(@offsetOf(Config, "dst_stride") == 12);
    std.debug.assert(@offsetOf(Config, "frame_drop") == 14);
    std.debug.assert(@offsetOf(Config, "bytes_per_pixel") == 15);
    std.debug.assert(@offsetOf(Config, "interrupts") == 16);
    std.debug.assert(@offsetOf(Config, "capture_format") == 20);
    std.debug.assert(@offsetOf(Config, "capture_mode") == 21);
    std.debug.assert(@offsetOf(Config, "first_field") == 29);
    std.debug.assert(@offsetOf(Config, "edge") == 30);
    std.debug.assert(@offsetOf(Config, "byte_swap") == 34);
    std.debug.assert(@offsetOf(Config, "scale") == 38);
    std.debug.assert(@offsetOf(Config, "interlace") == 50);
    std.debug.assert(@offsetOf(Config, "low_pass_filter") == 53);
    std.debug.assert(@offsetOf(Config, "image_area_size") == 56);
    std.debug.assert(@sizeOf(Config) == 60);

    std.debug.assert(@offsetOf(Status, "events") == 0);
    std.debug.assert(@offsetOf(Status, "data_size") == 4);
    std.debug.assert(@offsetOf(Status, "capturing") == 8);
    std.debug.assert(@offsetOf(Status, "reset_in_flight") == 9);
    std.debug.assert(@offsetOf(Status, "active_plane") == 10);
    std.debug.assert(@offsetOf(Status, "top_field") == 11);
    std.debug.assert(@sizeOf(Status) == 12);

    std.debug.assert(@offsetOf(Buffers, "y_top") == 0);
    std.debug.assert(@offsetOf(Buffers, "bundle_size_bytes") == ptr_bytes * 8);
    std.debug.assert(@sizeOf(Buffers) == std.mem.alignForward(usize, (ptr_bytes * 8) + 4, ptr_bytes));
}

/// Claim the CEU and program one capture descriptor: Closed to Idle.
pub extern fn ra8_ceu_init(cfg: *const Config) callconv(.c) u16;

/// Release the CEU claim: Idle to Closed.
pub extern fn ra8_ceu_deinit() callconv(.c) u16;

/// Software-reset an in-flight capture: Capturing to Idle.
pub extern fn ra8_ceu_reset() callconv(.c) u16;

/// Clear the named CETCR event latches.
pub extern fn ra8_ceu_clear_status(event_bits: u32) callconv(.c) u16;

/// Observe CETCR, CDSSR and CSTSR in one read.
pub extern fn ra8_ceu_status_snapshot(out_status: *Status) callconv(.c) u16;

/// Arm one capture against the supplied DMA addresses.
pub extern fn ra8_ceu_capture_start_ex(buffers: *const Buffers) callconv(.c) u16;
