//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Vectors for the service's StartRequest decoder, its Accepted encoder,
//! the terminated text copy, and the admit rule over them. Well-formed bytes
//! come from the reference protobuf encoder (Python google.protobuf over the
//! .proto).

const std = @import("std");
const implementation = @import("implementation");

const reply_encode = implementation.mdl_reply_encode;
const request_decode = implementation.mdl_request_decode;
const start = implementation.mdl_service_start;
const start_text = implementation.mdl_start_text;

fn hex(comptime text: []const u8) [text.len / 2]u8 {
    var out: [text.len / 2]u8 = undefined;
    _ = std.fmt.hexToBytes(&out, text) catch unreachable;
    return out;
}

const full_start = "0803122068747470733a2f2f6578616d706c652e746573742f626f6f6b2e7261626f6f6b18082205" ++
    "7261382f312a0f68747470733a2f2f722e746573742f320522616263223a1d5765642c2032312" ++
    "04f637420323031352030373a32383a303020474d5440b0ea01";
const min_start = "0803120968747470733a2f2f78";

test "start request from the reference encoder" {
    const view = try request_decode.start(&hex(full_start));
    try std.testing.expectEqual(@as(u32, 3), view.protocol_version);
    try std.testing.expectEqualStrings("https://example.test/book.rabook", view.url);
    try std.testing.expectEqual(@as(u32, 8), view.format);
    try std.testing.expectEqualStrings("ra8/1", view.user_agent);
    try std.testing.expectEqualStrings("https://r.test/", view.referer);
    try std.testing.expectEqualStrings("\"abc\"", view.if_none_match);
    try std.testing.expectEqualStrings("Wed, 21 Oct 2015 07:28:00 GMT", view.if_modified_since);
    try std.testing.expectEqual(@as(u32, 30000), view.timeout_ms);
}

test "start request leaves absent headers empty" {
    const view = try request_decode.start(&hex(min_start));
    try std.testing.expectEqualStrings("https://x", view.url);
    try std.testing.expectEqualStrings("", view.user_agent);
    try std.testing.expectEqual(@as(u32, 0), view.format);
}

test "start request with an unknown field is malformed" {
    try std.testing.expectError(error.Malformed, request_decode.start(&hex(min_start ++ "4801")));
}

test "accepted matches the reference encoder" {
    var buf: [start.accepted_max]u8 = undefined;
    try std.testing.expectEqualSlices(u8, &hex("080310071880082008"), try reply_encode.accepted(&buf, 7, 8));
    try std.testing.expectEqualSlices(u8, &hex("08031001188008"), try reply_encode.accepted(&buf, 1, 0));
}

test "the largest accepted fits its scratch exactly" {
    var buf: [start.accepted_max]u8 = undefined;
    const bytes = try reply_encode.accepted(&buf, std.math.maxInt(u32), 255);
    try std.testing.expectEqualSlices(u8, &hex("080310ffffffff0f18800820ff01"), bytes);
}

test "accepted refuses a short buffer and leaves it untouched" {
    var out: [8]u8 = @splat(0xee);
    try std.testing.expectError(error.NoSpace, start.accepted(7, 8, &out));
    try std.testing.expectEqualSlices(u8, &([_]u8{0xee} ** 8), &out);
}

test "admit copies every field terminated into the text storage" {
    var text: start_text.Text = .{};
    const request = try start.admit(&hex(full_start), false, &text);
    try std.testing.expectEqualStrings("https://example.test/book.rabook", std.mem.span(request.url.?));
    try std.testing.expectEqualStrings("ra8/1", std.mem.span(request.http.user_agent.?));
    try std.testing.expectEqualStrings("https://r.test/", std.mem.span(request.http.referer.?));
    try std.testing.expectEqualStrings("\"abc\"", std.mem.span(request.http.if_none_match.?));
    try std.testing.expectEqualStrings("Wed, 21 Oct 2015 07:28:00 GMT", std.mem.span(request.http.if_modified_since.?));
    try std.testing.expectEqual(@as(u8, 8), request.format);
    try std.testing.expectEqual(@as(u32, 30000), request.http.timeout_ms);
    try std.testing.expectEqual(@intFromPtr(&text.url), @intFromPtr(request.url.?));
}

test "admit gives absent headers as empty strings, never null" {
    var text: start_text.Text = .{};
    const request = try start.admit(&hex(min_start), false, &text);
    try std.testing.expectEqualStrings("", std.mem.span(request.http.user_agent.?));
    try std.testing.expectEqualStrings("", std.mem.span(request.http.if_modified_since.?));
}

test "admit refuses an http url as invalid" {
    var text: start_text.Text = .{};
    // StartRequest{protocol_version: 3, url: "http://x"}
    try std.testing.expectError(error.Invalid, start.admit(&hex("08031208687474703a2f2f78"), false, &text));
}

test "admit refuses a start while a job runs, after checking it, before copying" {
    var text: start_text.Text = .{};
    try std.testing.expectError(error.Busy, start.admit(&hex(min_start), true, &text));
    try std.testing.expectEqual(@as(u8, 0), text.url[0]);
    try std.testing.expectError(error.Malformed, start.admit(&hex(min_start ++ "4801"), true, &text));
}
