//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Zig validation and composition stage for the firmware pipeline.

/// The firmware pipeline C ABI: the config, result and status types this
/// stage validates against, plus the Rust summary entry point it calls.
/// Public so a caller of this module names the same types the C header does.
pub const c = @cImport({
    @cDefine("static_assert", "_Static_assert");
    @cDefine("alignof", "_Alignof");
    @cInclude("firmware_pipeline.h");
    @cInclude("firmware_pipeline_rust.h");
});

const max_image_size: usize = 16 * 1024 * 1024;
const zig_stage_marker: u8 = 0x5a;

fn xor8(data: []const u8) u8 {
    var result: u8 = 0;
    for (data) |byte| result ^= byte;
    return result;
}

/// Analyse one firmware image: validate the caller's config and size, run the
/// Rust summary stage, then fold in this stage's own xor8 and stage marker.
///
/// Rejects a null `config`, `data` or `out_result`, a config whose
/// `abi_version` is not the header's or whose `reserved0` is non-zero, and an
/// image larger than 16 MiB. A failing Rust stage is forwarded unchanged.
/// `out_result` is written only on `k_firmware_pipeline_ok`.
pub export fn firmware_pipeline_analyze(
    config: ?*const c.firmware_pipeline_config_t,
    data: ?[*]const u8,
    size: usize,
    out_result: ?*c.firmware_pipeline_result_t,
) callconv(.c) c.firmware_pipeline_status_t {
    if (config == null or data == null or out_result == null) {
        return c.k_firmware_pipeline_invalid_argument;
    }
    if (config.?.abi_version != c.k_firmware_pipeline_abi_version or config.?.reserved0 != 0) {
        return c.k_firmware_pipeline_invalid_argument;
    }
    if (size > max_image_size) return c.k_firmware_pipeline_invalid_size;

    var rust_summary: c.firmware_pipeline_rust_summary_t = undefined;
    const rust_status = c.firmware_pipeline_rust_analyze(data, size, &rust_summary);
    if (rust_status != c.k_firmware_pipeline_ok) return rust_status;

    const image = data.?[0..size];
    var combined = std.mem.zeroes(c.firmware_pipeline_result_t);
    combined.byte_count = rust_summary.byte_count;
    combined.zero_count = rust_summary.zero_count;
    combined.erased_count = rust_summary.erased_count;
    combined.fnv1a64 = rust_summary.fnv1a64;
    combined.zig_xor8 = xor8(image);
    combined.zig_stage_marker = zig_stage_marker;
    out_result.?.* = combined;
    return c.k_firmware_pipeline_ok;
}

const std = @import("std");
