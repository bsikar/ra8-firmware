//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Standalone type definitions for checking `src/adapter.zig` outside translate-c.

pub const firmware_pipeline_status_t = i32;
pub const k_firmware_pipeline_ok: firmware_pipeline_status_t = 0;
pub const k_firmware_pipeline_invalid_argument: firmware_pipeline_status_t = 1;
pub const k_firmware_pipeline_invalid_size: firmware_pipeline_status_t = 2;
pub const k_firmware_pipeline_empty_image: firmware_pipeline_status_t = 3;

pub const k_firmware_pipeline_abi_version: u32 = 0x5A495001;

pub const firmware_pipeline_config_t = extern struct {
    abi_version: u32 = 0,
    reserved0: u32 = 0,
};

pub const firmware_pipeline_result_t = extern struct {
    byte_count: u64 = 0,
    zero_count: u64 = 0,
    erased_count: u64 = 0,
    fnv1a64: u64 = 0,
    zig_xor8: u8 = 0,
    zig_stage_marker: u8 = 0,
    reserved: [6]u8 = .{ 0, 0, 0, 0, 0, 0 },
};

pub const firmware_pipeline_rust_summary_t = extern struct {
    byte_count: u64 = 0,
    zero_count: u64 = 0,
    erased_count: u64 = 0,
    fnv1a64: u64 = 0,
};

pub extern fn firmware_pipeline_rust_analyze(
    data: ?[*]const u8,
    size: usize,
    out_summary: ?*firmware_pipeline_rust_summary_t,
) callconv(.c) firmware_pipeline_status_t;
