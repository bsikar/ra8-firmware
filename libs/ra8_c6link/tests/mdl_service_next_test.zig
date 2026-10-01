//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Vectors for the service's NextRequest decoder, its Chunk encoder, the
//! worst-case Chunk bound, and the admit/pack rules over them. Well-formed
//! bytes and the worst-case sizes come from the reference protobuf encoder
//! (Python google.protobuf over the .proto).

const std = @import("std");
const implementation = @import("implementation");

const bound = implementation.mdl_chunk_bound;
const next = implementation.mdl_service_next;
const pull = implementation.mdl_pull;
const reply_encode = implementation.mdl_reply_encode;
const request_decode = implementation.mdl_request_decode;
const rules = implementation.mdl_service_rules;

fn hex(comptime text: []const u8) [text.len / 2]u8 {
    var out: [text.len / 2]u8 = undefined;
    _ = std.fmt.hexToBytes(&out, text) catch unreachable;
    return out;
}

const live: pull.JobView = .{ .next_offset = 2048, .active_job_id = 7, .active = true };
const next_2048 = "08031007188010208008";
const data_chunk = "0803100718032080102a0568656c6c6f3080203802";
const terminal_chunk = "08031007180420802030802038034a20" ++ "ab" ** 32 ++
    "50c801620522616263226a1d5765642c203231204f637420323031352030373a32383a303020474d54" ++
    "720f6170706c69636174696f6e2f7a6970";

test "next request from the reference encoder" {
    const view = try request_decode.next(&hex(next_2048));
    try std.testing.expectEqual(@as(u32, 3), view.protocol_version);
    try std.testing.expectEqual(@as(u32, 7), view.job_id);
    try std.testing.expectEqual(@as(u64, 2048), view.acknowledged_offset);
    try std.testing.expectEqual(@as(u32, 1024), view.max_bytes);
}

test "next request with an unknown field is malformed" {
    try std.testing.expectError(error.Malformed, request_decode.next(&hex(next_2048 ++ "2801")));
}

test "data chunk matches the reference encoder" {
    var buf: [64]u8 = undefined;
    const bytes = try reply_encode.chunk(&buf, .{ .job_id = 7, .sequence = 3, .offset = 2048, .data = "hello", .total = 4096 });
    try std.testing.expectEqualSlices(u8, &hex(data_chunk), bytes);
}

test "empty data chunk leaves every default off the wire" {
    var buf: [16]u8 = undefined;
    const bytes = try reply_encode.chunk(&buf, .{ .job_id = 7, .sequence = 0, .offset = 0, .data = "", .total = 0 });
    try std.testing.expectEqualSlices(u8, &hex("080310073802"), bytes);
}

test "terminal chunk matches the reference encoder" {
    var buf: [256]u8 = undefined;
    const bytes = try reply_encode.chunk(&buf, .{
        .job_id = 7,
        .sequence = 4,
        .offset = 4096,
        .data = "",
        .total = 4096,
        .terminal = .{
            .sha256 = &([_]u8{0xab} ** 32),
            .http_status = 200,
            .etag = "\"abc\"",
            .last_modified = "Wed, 21 Oct 2015 07:28:00 GMT",
            .content_type = "application/zip",
        },
    });
    try std.testing.expectEqualSlices(u8, &hex(terminal_chunk), bytes);
}

test "worst-case bounds match the reference encoder" {
    try std.testing.expectEqual(@as(usize, 42), bound.data(0));
    try std.testing.expectEqual(@as(usize, 45), bound.data(1));
    try std.testing.expectEqual(@as(usize, 171), bound.data(127));
    try std.testing.expectEqual(@as(usize, 173), bound.data(128));
    try std.testing.expectEqual(@as(usize, 1069), bound.data(1024));
    try std.testing.expectEqual(@as(usize, 467), bound.terminal);
    try std.testing.expectEqual(@as(usize, 467), bound.worstCase(1));
    try std.testing.expectEqual(@as(usize, 1069), bound.worstCase(1024));
}

test "admit grants the requested bound when the worst reply fits" {
    try std.testing.expectEqual(@as(u32, 1024), try next.admit(&hex(next_2048), &live, 1069));
}

test "admit refuses a buffer one byte short of the worst reply" {
    try std.testing.expectError(error.NoSpace, next.admit(&hex(next_2048), &live, 1068));
}

test "admit refuses a request at the wrong offset" {
    const behind: pull.JobView = .{ .next_offset = 0, .active_job_id = 7, .active = true };
    try std.testing.expectError(error.Uncorrelated, next.admit(&hex(next_2048), &behind, 2048));
}

test "admit refuses a truncated request as malformed" {
    try std.testing.expectError(error.Malformed, next.admit(&hex("0803108f"), &live, 2048));
}

test "pack encodes a data pull" {
    var buf: [1069]u8 = undefined;
    const view: next.ReplyView = .{ .offset = 2048, .total = 4096, .data = "hello", .job_id = 7, .sequence = 3, .got = 5 };
    try std.testing.expectEqualSlices(u8, &hex(data_chunk), try next.pack(&view, &buf));
}

test "pack encodes a terminal pull from fixed response storage" {
    var response: rules.ResponseView = .{ .status = 200 };
    @memcpy(response.etag[0..5], "\"abc\"");
    @memcpy(response.last_modified[0..29], "Wed, 21 Oct 2015 07:28:00 GMT");
    @memcpy(response.content_type[0..15], "application/zip");
    const digest = [_]u8{0xab} ** 32;
    const view: next.ReplyView = .{ .offset = 4096, .total = 4096, .digest = &digest, .response = &response, .job_id = 7, .sequence = 4, .complete = true };
    var buf: [467]u8 = undefined;
    try std.testing.expectEqualSlices(u8, &hex(terminal_chunk), try next.pack(&view, &buf));
}

test "pack refuses a terminal pull with no digest" {
    const response: rules.ResponseView = .{ .status = 200 };
    const view: next.ReplyView = .{ .response = &response, .job_id = 7, .complete = true };
    var buf: [467]u8 = undefined;
    try std.testing.expectError(error.Missing, next.pack(&view, &buf));
}

test "pack refuses a buffer smaller than the chunk" {
    const view: next.ReplyView = .{ .offset = 2048, .total = 4096, .data = "hello", .job_id = 7, .sequence = 3, .got = 5 };
    var buf: [8]u8 = undefined;
    try std.testing.expectError(error.NoSpace, next.pack(&view, &buf));
}
