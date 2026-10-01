//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The CEU descriptor types this board fills in, mirrored from
//! `ra8_ceu_types.h`, plus the register encodings it selects. Same role
//! `clock_types.zig` plays for the chip clock binding: the layout has to match
//! the C exactly, so it lives in one file rather than being spread over the
//! code that populates it.

/// `ra8_ceu_edge_info_t`. Each field is `ra8_ceu_edge_t`, `enum : uint8_t`.
pub const EdgeInfo = extern struct {
    data: u8,
    hsync: u8,
    vsync: u8,
    field: u8,
};

/// `ra8_ceu_byte_swap_t`.
pub const ByteSwap = extern struct {
    swap_8_bit: bool,
    swap_16_bit: bool,
    swap_32_bit: bool,
};

/// `ra8_ceu_scale_t`. All six fields zero means clip-only, no scale-down.
pub const Scale = extern struct {
    h_mantissa: u16 = 0,
    h_fraction: u16 = 0,
    v_mantissa: u16 = 0,
    v_fraction: u16 = 0,
    h_output_clip: u16 = 0,
    v_output_clip: u16 = 0,
};

/// `ra8_ceu_config_t`. The ten encoding fields between `interrupts` and `edge`
/// are each `enum : uint8_t` in the header.
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
    edge: EdgeInfo = .{ .data = 0, .hsync = 0, .vsync = 0, .field = 0 },
    byte_swap: ByteSwap = .{ .swap_8_bit = false, .swap_16_bit = false, .swap_32_bit = false },
    scale: Scale = .{},
    interlace: bool = false,
    one_field_only: bool = false,
    bundle_write: bool = false,
    low_pass_filter: bool = false,
    image_area_size: u32 = 0,
};

/// `ra8_board_camera_ceu_config_t`: the HAL descriptor plus the policy the
/// board publishes alongside it.
pub const BoardConfig = extern struct {
    ceu: Config,
    frame_bytes_max: u32,
    stride_bytes: u32,
    poll_interval_ms: u32,
    poll_attempts: u32,
    xclk_hz: u32,
    settle_ms: u32,
    width_px: u16,
    height_px: u16,
};

/// The `ra8_ceu_*` encodings this board selects.
pub const Ceu = struct {
    /// Raw synchronous data fetch.
    pub const fmt_data_synchronous: u8 = 1;
    /// JPEG / data-enable fetch.
    pub const fmt_data_enable: u8 = 2;
    /// CE auto-clears after CPE.
    pub const capture_single: u8 = 0;
    /// VIO_D[7:0] used.
    pub const bus_8_bit: u8 = 0;
    pub const pol_high_active: u8 = 0;
    /// Sample on the rising VIO_CLK edge.
    pub const edge_rising: u8 = 0;
    pub const input_cb0_y0_cr0_y1: u8 = 0;
    /// Pass-through, no colour conversion.
    pub const output_ycbcr_422: u8 = 1;
    /// Capture starts at the next VD.
    pub const field_immediate: u8 = 0;
    pub const burst_32: u8 = 0;
    pub const burst_256: u8 = 3;
};
