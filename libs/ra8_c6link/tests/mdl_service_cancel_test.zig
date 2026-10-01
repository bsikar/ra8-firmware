//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Vectors for the service's CancelRequest decoder, its Cancelled encoder,
//! and the reply rule over both. Well-formed bytes come from the reference
//! protobuf encoder (Python google.protobuf over the .proto).

const std = @import("std");
const implementation = @import("implementation");

const cancel = implementation.mdl_service_cancel;
const pull = implementation.mdl_pull;
const reply_encode = implementation.mdl_reply_encode;
const request_decode = implementation.mdl_request_decode;

fn hex(comptime text: []const u8) [text.len / 2]u8 {
    var out: [text.len / 2]u8 = undefined;
    _ = std.fmt.hexToBytes(&out, text) catch unreachable;
    return out;
}

const live: pull.JobView = .{ .active_job_id = 7, .active = true };

test "cancel request from the reference encoder" {
    const view = try request_decode.cancel(&hex("08031007"));
    try std.testing.expectEqual(@as(u32, 3), view.protocol_version);
    try std.testing.expectEqual(@as(u32, 7), view.job_id);
}

test "cancel request with an unknown field is malformed" {
    try std.testing.expectError(error.Malformed, request_decode.cancel(&hex("080310071801")));
}

test "cancel request cut mid-varint is malformed" {
    try std.testing.expectError(error.Malformed, request_decode.cancel(&hex("0803108f")));
}

test "cancelled matches the reference encoder" {
    var buf: [cancel.reply_max]u8 = undefined;
    try std.testing.expectEqualSlices(u8, &hex("08031007"), try reply_encode.cancelled(&buf, 7));
    try std.testing.expectEqualSlices(u8, &hex("080310ffffffff0f"), try reply_encode.cancelled(&buf, 0xFFFF_FFFF));
}

test "the widest cancelled fits reply_max" {
    var buf: [cancel.reply_max]u8 = undefined;
    try std.testing.expectEqual(cancel.reply_max, (try reply_encode.cancelled(&buf, 0xFFFF_FFFF)).len);
}

test "reply acknowledges the active job" {
    var out: [16]u8 = undefined;
    try std.testing.expectEqualSlices(u8, &hex("08031007"), try cancel.reply(&hex("08031007"), &live, &out));
}

test "reply refuses another job, a stale version and an idle service" {
    var out: [16]u8 = undefined;
    try std.testing.expectError(error.Uncorrelated, cancel.reply(&hex("08031008"), &live, &out));
    try std.testing.expectError(error.Uncorrelated, cancel.reply(&hex("08021007"), &live, &out));
    const idle: pull.JobView = .{ .active_job_id = 7 };
    try std.testing.expectError(error.Uncorrelated, cancel.reply(&hex("08031007"), &idle, &out));
}

test "reply refuses a malformed request before correlating" {
    var out: [16]u8 = undefined;
    try std.testing.expectError(error.Malformed, cancel.reply(&hex("080310071801"), &live, &out));
}

test "reply leaves a too-small buffer untouched" {
    var out = [_]u8{0xEE} ** 3;
    try std.testing.expectError(error.NoSpace, cancel.reply(&hex("08031007"), &live, &out));
    try std.testing.expectEqualSlices(u8, &[_]u8{0xEE} ** 3, &out);
}
