//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for `ra8_vfs_compress_abi`.

const std = @import("std");
const vfs = @import("vfs_compress");

test "write rejects null pointers" {
    var blob: [16]u8 = undefined;
    var scratch: [8]u8 = undefined;
    var out_len: u32 = 0;
    try std.testing.expectEqual(vfs.k_ra8_err_null_ptr, vfs.ra8_vfs_compress_write(null, &blob, 1, &scratch, 8, &blob, 16, &out_len));
    try std.testing.expectEqual(vfs.k_ra8_err_null_ptr, vfs.ra8_vfs_compress_write("ram:/a", null, 1, &scratch, 8, &blob, 16, &out_len));
    try std.testing.expectEqual(vfs.k_ra8_err_null_ptr, vfs.ra8_vfs_compress_write("ram:/a", &blob, 1, null, 8, &blob, 16, &out_len));
    try std.testing.expectEqual(vfs.k_ra8_err_null_ptr, vfs.ra8_vfs_compress_write("ram:/a", &blob, 1, &scratch, 8, null, 16, &out_len));
    try std.testing.expectEqual(vfs.k_ra8_err_null_ptr, vfs.ra8_vfs_compress_write("ram:/a", &blob, 1, &scratch, 8, &blob, 16, null));
}

test "read rejects null pointers" {
    var blob: [16]u8 = undefined;
    var out: [16]u8 = undefined;
    var out_len: u32 = 0;
    try std.testing.expectEqual(vfs.k_ra8_err_null_ptr, vfs.ra8_vfs_compress_read(null, &blob, 16, &out, 16, &out_len));
    try std.testing.expectEqual(vfs.k_ra8_err_null_ptr, vfs.ra8_vfs_compress_read("ram:/a", null, 16, &out, 16, &out_len));
    try std.testing.expectEqual(vfs.k_ra8_err_null_ptr, vfs.ra8_vfs_compress_read("ram:/a", &blob, 16, null, 16, &out_len));
    try std.testing.expectEqual(vfs.k_ra8_err_null_ptr, vfs.ra8_vfs_compress_read("ram:/a", &blob, 16, &out, 16, null));
}

test "round trip through the stubbed VFS" {
    var payload: [24]u8 = undefined;
    for (&payload, 0..) |*b, i| b.* = @intCast(i * 7 + 1);
    var blob: [64]u8 = undefined;
    var scratch: [8]u8 = undefined;
    var blob_len: u32 = 0;
    var restored: [32]u8 = undefined;
    var out_len: u32 = 0;

    try std.testing.expectEqual(vfs.k_ra8_ok, vfs.ra8_vfs_compress_write("ram:/STORY.RBK", &payload, payload.len, &scratch, scratch.len, &blob, blob.len, &blob_len));
    try std.testing.expectEqual(@as(u32, payload.len + 4), blob_len);
    try std.testing.expectEqual(vfs.k_ra8_ok, vfs.ra8_vfs_compress_read("ram:/STORY.RBK", &blob, blob.len, &restored, restored.len, &out_len));
    try std.testing.expectEqual(payload.len, out_len);
    try std.testing.expectEqualSlices(u8, &payload, restored[0..out_len]);
}

test "blob filling blob_cap exactly is refused as invalid_size" {
    var payload: [16]u8 = undefined;
    @memset(&payload, 0x42);
    var blob: [24]u8 = undefined;
    var scratch: [8]u8 = undefined;
    var blob_len: u32 = 0;
    var out: [16]u8 = undefined;
    var out_len: u32 = 0;
    // The stub codec writes 20 bytes for this payload; a read whose blob_cap
    // equals the on-disk size hits the truncation refusal.
    try std.testing.expectEqual(vfs.k_ra8_ok, vfs.ra8_vfs_compress_write("ram:/FULL", &payload, payload.len, &scratch, scratch.len, &blob, blob.len, &blob_len));
    try std.testing.expectEqual(@as(u32, 20), blob_len);
    try std.testing.expectEqual(vfs.k_ra8_err_invalid_size, vfs.ra8_vfs_compress_read("ram:/FULL", &blob, blob_len, &out, out.len, &out_len));
}

test "missing mount propagates not_found" {
    var payload: [4]u8 = .{ 1, 2, 3, 4 };
    var blob: [32]u8 = undefined;
    var scratch: [8]u8 = undefined;
    var blob_len: u32 = 0;
    var out: [32]u8 = undefined;
    var out_len: u32 = 0;
    // The stub VFS refuses "absent:/..." with k_ra8_err_not_found (0x106).
    try std.testing.expectEqual(@as(vfs.ra8_err_t, 0x106), vfs.ra8_vfs_compress_write("absent:/X", &payload, payload.len, &scratch, scratch.len, &blob, blob.len, &blob_len));
    try std.testing.expectEqual(@as(vfs.ra8_err_t, 0x106), vfs.ra8_vfs_compress_read("absent:/X", &blob, blob.len, &out, out.len, &out_len));
}
