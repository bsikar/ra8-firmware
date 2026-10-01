//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The Chunk decoder against bytes from the reference protobuf encoder
//! (python protobuf over `proto/ra8_media_download.proto`), plus the
//! malformed shapes protobuf-c refused.

const std = @import("std");

const implementation = @import("implementation");
const decode = implementation.mdl_chunk_decode;
const types = implementation.mdl_types;

fn hex(comptime text: []const u8) [text.len / 2]u8 {
    var out: [text.len / 2]u8 = undefined;
    _ = std.fmt.hexToBytes(&out, text) catch unreachable;
    return out;
}

fn str(text: ?[]const u8) []const u8 {
    return text.?;
}

test "a downloading chunk decodes its key and body" {
    const bytes = hex("0803100718012080082a0568656c6c6f3080403802");
    const got = try decode.chunk(&bytes);
    try std.testing.expectEqual(@as(u32, 3), got.key.protocol_version);
    try std.testing.expectEqual(@as(u32, 7), got.key.job_id);
    try std.testing.expectEqual(@as(u32, 1), got.key.sequence);
    try std.testing.expectEqual(@as(u64, 1024), got.key.offset);
    try std.testing.expectEqual(@as(u32, 5), got.key.data_len);
    try std.testing.expect(got.key.data_present);
    try std.testing.expectEqual(@as(u32, 0), got.key.unknown_fields);
    try std.testing.expectEqualStrings("hello", got.view.data.?);
    try std.testing.expectEqual(@as(u64, 8192), got.view.total_bytes);
    try std.testing.expectEqual(types.State.downloading, got.view.state);
    try std.testing.expect(got.view.sha256 == null);
}

test "absent headers decode as empty, not unset" {
    const bytes = hex("0803100718012080082a0568656c6c6f3080403802");
    const got = try decode.chunk(&bytes);
    try std.testing.expectEqual(@as(usize, 0), str(got.view.etag).len);
    try std.testing.expectEqual(@as(usize, 0), str(got.view.content_type).len);
}

test "a complete chunk decodes its digest and headers" {
    const bytes = hex("08031007180220804030804038034a20" ++ "ab" ** 32 ++
        "50c8015a0135620522616263226a1d5765642c203231204f637420323031352030373a32383a303020474d54720f6170706c69636174696f6e2f7a6970");
    const got = try decode.chunk(&bytes);
    try std.testing.expectEqual(types.State.complete, got.view.state);
    try std.testing.expectEqual(@as(usize, 32), got.view.sha256.?.len);
    try std.testing.expectEqual(@as(u8, 0xAB), got.view.sha256.?[31]);
    try std.testing.expectEqual(@as(i32, 200), got.view.http_status);
    try std.testing.expectEqualStrings("5", str(got.view.retry_after));
    try std.testing.expectEqualStrings("\"abc\"", str(got.view.etag));
    try std.testing.expectEqualStrings("Wed, 21 Oct 2015 07:28:00 GMT", str(got.view.last_modified));
    try std.testing.expectEqualStrings("application/zip", str(got.view.content_type));
    try std.testing.expect(!got.key.data_present);
}

test "a failed chunk decodes its status" {
    const got = try decode.chunk(&hex("080310073805408802"));
    try std.testing.expectEqual(types.State.failed, got.view.state);
    try std.testing.expectEqual(@as(i32, 0x108), got.view.status);
}

test "wide and negative values decode at full width" {
    const got = try decode.chunk(&hex("080310ffffffff0f20858080808020308180808080808080800140fbffffffffffffffff0150ffffffffffffffffff01"));
    try std.testing.expectEqual(@as(u32, 0xFFFFFFFF), got.view.job_id);
    try std.testing.expectEqual(@as(u64, (1 << 40) + 5), got.view.offset);
    try std.testing.expectEqual(@as(u64, (1 << 63) + 1), got.view.total_bytes);
    try std.testing.expectEqual(@as(i32, -5), got.view.status);
    try std.testing.expectEqual(@as(i32, -1), got.view.http_status);
}

test "an empty body is a null span, as protobuf-c left it" {
    const got = try decode.chunk(&hex("2a00"));
    try std.testing.expect(got.view.data == null);
    try std.testing.expect(!got.key.data_present);
}

test "a repeated field keeps its last value" {
    const got = try decode.chunk(&hex("10071009"));
    try std.testing.expectEqual(@as(u32, 9), got.key.job_id);
}

test "unknown fields are counted" {
    const got = try decode.chunk(&hex("7801800101"));
    try std.testing.expectEqual(@as(u32, 2), got.key.unknown_fields);
}

test "a state outside a byte is refused rather than cut to one" {
    try std.testing.expectError(error.Malformed, decode.chunk(&hex("388202")));
    try std.testing.expectError(error.Malformed, decode.chunk(&hex("38ffffffffffffffffff01")));
}

test "a known field on the wrong wire type is refused" {
    try std.testing.expectError(error.Malformed, decode.chunk(&hex("2a")));
    try std.testing.expectError(error.Malformed, decode.chunk(&hex("2801")));
    try std.testing.expectError(error.Malformed, decode.chunk(&hex("220100")));
    try std.testing.expectError(error.Malformed, decode.chunk(&hex("6201")));
}

test "a body that runs past the buffer is refused" {
    try std.testing.expectError(error.Malformed, decode.chunk(&hex("2a0568656c")));
}

test "the empty message decodes to defaults" {
    const got = try decode.chunk(&.{});
    try std.testing.expectEqual(@as(u32, 0), got.key.protocol_version);
    try std.testing.expectEqual(@as(u8, 0), got.view.state);
}
