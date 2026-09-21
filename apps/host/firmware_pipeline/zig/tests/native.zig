//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Native Zig and Zig-to-Rust membrane tests for the firmware pipeline.

const std = @import("std");
const adapter = @import("adapter");
const c = adapter.c;

fn sentinel() c.firmware_pipeline_result_t {
    return std.mem.bytesToValue(c.firmware_pipeline_result_t, &([_]u8{0xa5} ** @sizeOf(c.firmware_pipeline_result_t)));
}

test "Zig and Rust stages publish a combined result" {
    const input = [_]u8{ 0, 0xff, 7 };
    const config = c.firmware_pipeline_config_t{
        .abi_version = c.k_firmware_pipeline_abi_version,
        .reserved0 = 0,
    };
    var result = sentinel();
    try std.testing.expectEqual(c.k_firmware_pipeline_ok, adapter.firmware_pipeline_analyze(&config, &input, input.len, &result));
    try std.testing.expectEqual(@as(u64, 3), result.byte_count);
    try std.testing.expectEqual(@as(u64, 1), result.zero_count);
    try std.testing.expectEqual(@as(u64, 1), result.erased_count);
    try std.testing.expectEqual(@as(u8, 0xf8), result.zig_xor8);
    try std.testing.expectEqual(@as(u8, 0x5a), result.zig_stage_marker);
}

fn expectPreserved(
    expected: c.firmware_pipeline_status_t,
    config: ?*const c.firmware_pipeline_config_t,
    data: ?[*]const u8,
    size: usize,
) !void {
    var result = sentinel();
    const saved = result;
    try std.testing.expectEqual(expected, adapter.firmware_pipeline_analyze(config, data, size, &result));
    try std.testing.expectEqualSlices(u8, std.mem.asBytes(&saved), std.mem.asBytes(&result));
}

test "each Zig pointer guard independently preserves output" {
    const input = [_]u8{1};
    const config = c.firmware_pipeline_config_t{
        .abi_version = c.k_firmware_pipeline_abi_version,
        .reserved0 = 0,
    };
    try expectPreserved(c.k_firmware_pipeline_invalid_argument, null, &input, input.len);
    try expectPreserved(c.k_firmware_pipeline_invalid_argument, &config, null, input.len);

    var result = sentinel();
    const saved = result;
    try std.testing.expectEqual(c.k_firmware_pipeline_invalid_argument, adapter.firmware_pipeline_analyze(&config, &input, input.len, null));
    try std.testing.expectEqualSlices(u8, std.mem.asBytes(&saved), std.mem.asBytes(&result));
}

test "each Zig value guard independently preserves output" {
    const input = [_]u8{1};
    const invalid_version = c.firmware_pipeline_config_t{ .abi_version = 0, .reserved0 = 0 };
    const invalid_reserved = c.firmware_pipeline_config_t{
        .abi_version = c.k_firmware_pipeline_abi_version,
        .reserved0 = 1,
    };
    const valid = c.firmware_pipeline_config_t{
        .abi_version = c.k_firmware_pipeline_abi_version,
        .reserved0 = 0,
    };
    try expectPreserved(c.k_firmware_pipeline_invalid_argument, &invalid_version, &input, input.len);
    try expectPreserved(c.k_firmware_pipeline_invalid_argument, &invalid_reserved, &input, input.len);
    try expectPreserved(c.k_firmware_pipeline_invalid_size, &valid, &input, 16 * 1024 * 1024 + 1);
}

test "Rust empty-image failure preserves output" {
    const placeholder = [_]u8{0};
    const config = c.firmware_pipeline_config_t{
        .abi_version = c.k_firmware_pipeline_abi_version,
        .reserved0 = 0,
    };
    var result = sentinel();
    const saved = result;
    try std.testing.expectEqual(c.k_firmware_pipeline_empty_image, adapter.firmware_pipeline_analyze(&config, &placeholder, 0, &result));
    try std.testing.expectEqualSlices(u8, std.mem.asBytes(&saved), std.mem.asBytes(&result));
}
