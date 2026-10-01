//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Vectors for the Accepted and Cancelled decoders and the wire reader under
//! them. Well-formed inputs were produced by the reference protobuf encoder
//! (Python google.protobuf over `proto/ra8_media_download.proto`); the
//! malformed ones are hand-built against protobuf-c's own scan rules.

const std = @import("std");
const implementation = @import("implementation");

const decode = implementation.mdl_decode;
const read = implementation.mdl_wire_read;

fn hex(comptime text: []const u8) [text.len / 2]u8 {
    var out: [text.len / 2]u8 = undefined;
    _ = std.fmt.hexToBytes(&out, text) catch unreachable;
    return out;
}

test "accepted from the reference encoder" {
    const view = try decode.accepted(&hex("080310071880082001"));
    try std.testing.expectEqual(@as(u32, 3), view.protocol_version);
    try std.testing.expectEqual(@as(u32, 7), view.job_id);
    try std.testing.expectEqual(@as(u32, 1024), view.max_chunk_bytes);
    try std.testing.expectEqual(@as(u32, 1), view.format);
    try std.testing.expectEqual(@as(u32, 0), view.unknown_fields);
}

test "accepted with a 32-bit job id and the last format" {
    const view = try decode.accepted(&hex("080310ffffffff0f18012008"));
    try std.testing.expectEqual(@as(u32, 0xFFFFFFFF), view.job_id);
    try std.testing.expectEqual(@as(u32, 1), view.max_chunk_bytes);
    try std.testing.expectEqual(@as(u32, 8), view.format);
}

test "cancelled from the reference encoder" {
    const view = try decode.cancelled(&hex("080310ac02"));
    try std.testing.expectEqual(@as(u32, 3), view.protocol_version);
    try std.testing.expectEqual(@as(u32, 300), view.job_id);
    try std.testing.expectEqual(@as(i32, 0), view.status);
    try std.testing.expectEqual(@as(u32, 0), view.unknown_fields);
}

test "a negative status travels as a ten-byte varint" {
    const view = try decode.cancelled(&hex("080310ac0218fbffffffffffffffff01"));
    try std.testing.expectEqual(@as(i32, -5), view.status);
}

test "an empty body decodes to every default, as protobuf-c did" {
    const view = try decode.accepted(&.{});
    try std.testing.expectEqual(@as(u32, 0), view.protocol_version);
    try std.testing.expectEqual(@as(u32, 0), view.unknown_fields);
}

test "unknown fields of every wire type are skipped and counted" {
    const bytes = hex("080310ac02" ++ "4801" ++ "5203616263" ++ "5d01020304" ++ "610102030405060708");
    const view = try decode.cancelled(&bytes);
    try std.testing.expectEqual(@as(u32, 300), view.job_id);
    try std.testing.expectEqual(@as(u32, 4), view.unknown_fields);
}

test "a repeated field keeps its last value" {
    const view = try decode.accepted(&hex("080310071009"));
    try std.testing.expectEqual(@as(u32, 9), view.job_id);
}

test "a known field on the wrong wire type is malformed" {
    try std.testing.expectError(error.Malformed, decode.accepted(&hex("0803120107")));
    try std.testing.expectError(error.Malformed, decode.cancelled(&hex("08031d01000000")));
}

test "field number zero is malformed" {
    try std.testing.expectError(error.Malformed, decode.accepted(&hex("0001")));
}

test "group wire types are malformed" {
    try std.testing.expectError(error.Malformed, decode.accepted(&hex("0b")));
    try std.testing.expectError(error.Malformed, decode.accepted(&hex("0c")));
}

test "a varint that does not end is malformed" {
    try std.testing.expectError(error.Malformed, decode.accepted(&hex("0883")));
    try std.testing.expectError(error.Malformed, decode.accepted(&hex("08" ++ "ff" ** 10 ++ "01")));
}

test "a ten-byte varint is the longest accepted, keeping its low 32 bits" {
    const view = try decode.accepted(&hex("08" ++ "ff" ** 9 ++ "01"));
    try std.testing.expectEqual(@as(u32, 0xFFFFFFFF), view.protocol_version);
}

test "a value running past the buffer is malformed" {
    try std.testing.expectError(error.Malformed, decode.accepted(&hex("080352056162")));
    try std.testing.expectError(error.Malformed, decode.accepted(&hex("08035d0102")));
    try std.testing.expectError(error.Malformed, decode.accepted(&hex("080361010203")));
}

test "a tag longer than five bytes is malformed" {
    try std.testing.expectError(error.Malformed, decode.accepted(&hex("f8ffffffff01")));
}

test "the reader stops exactly at the end of the buffer" {
    var reader = read.Reader.init(&hex("0803"));
    const field = (try reader.next()).?;
    try std.testing.expectEqual(@as(u32, 1), field.number);
    try std.testing.expectEqual(@as(u32, 3), try field.uint32());
    try std.testing.expectEqual(@as(?read.Field, null), try reader.next());
}
