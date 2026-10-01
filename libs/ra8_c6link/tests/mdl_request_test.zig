//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Contract tests for the caller-facing half of the media download client:
//! the optional-header bounds, the start-request argument contract, and the
//! slices the encoder reads.

const std = @import("std");

const implementation = @import("implementation");
const req = implementation.mdl_request;
const types = implementation.mdl_types;

fn z(comptime text: [:0]const u8) [*:0]const u8 {
    return text.ptr;
}

test "an absent optional field is valid" {
    try std.testing.expect(req.httpFieldValid(null, 16));
}

test "a bounded single-line field is valid" {
    try std.testing.expect(req.httpFieldValid(z("ra8/1.0"), 16));
    try std.testing.expect(req.httpFieldValid(z(""), 16));
}

test "a field that fills its bound without terminating is refused" {
    const exact: [*:0]const u8 = "abcdef";
    try std.testing.expect(req.httpFieldValid(exact, 7));
    try std.testing.expect(!req.httpFieldValid(exact, 6));
}

test "carriage return and newline are refused wherever they sit" {
    try std.testing.expect(!req.httpFieldValid(z("ra8\r\nX-Evil: 1"), 64));
    try std.testing.expect(!req.httpFieldValid(z("\n"), 64));
    try std.testing.expect(!req.httpFieldValid(z("trailing\r"), 64));
}

fn request(url: ?[*:0]const u8) types.Request {
    return .{ .url = url, .format = types.Format.loose, .http = .{} };
}

test "a plain https request is accepted and reports its url length" {
    const r = request(z("https://example.test/book.rabook"));
    try std.testing.expectEqual(@as(usize, 32), try req.startRequestValid(&r));
}

test "a null request or url is a null-pointer refusal" {
    try std.testing.expectError(error.NullPtr, req.startRequestValid(null));
    const r = request(null);
    try std.testing.expectError(error.NullPtr, req.startRequestValid(&r));
}

test "the scheme must be https and must carry an origin" {
    for ([_][*:0]const u8{
        z("http://example.test/x"),
        z("ftp://example.test/x"),
        z("https://"),
        z(""),
    }) |url| {
        const r = request(url);
        try std.testing.expectError(error.InvalidArg, req.startRequestValid(&r));
    }
}

test "a url that fails to terminate inside its bound is refused" {
    var buffer: [req.Bound.url + 8]u8 = @splat('a');
    @memcpy(buffer[0..8], "https://");
    buffer[req.Bound.url] = 0;
    const over = request(@ptrCast(&buffer));
    try std.testing.expectError(error.InvalidArg, req.startRequestValid(&over));

    buffer[req.Bound.url - 1] = 0;
    const edge = request(@ptrCast(&buffer));
    try std.testing.expectEqual(@as(usize, req.Bound.url - 1), try req.startRequestValid(&edge));
}

test "an unknown format is refused" {
    var r = request(z("https://example.test/x"));
    r.format = types.Format.rabook;
    _ = try req.startRequestValid(&r);
    r.format = types.Format.rabook + 1;
    try std.testing.expectError(error.InvalidArg, req.startRequestValid(&r));
    r.format = types.Format.invalid;
    try std.testing.expectError(error.InvalidArg, req.startRequestValid(&r));
}

test "the timeout ceiling is inclusive" {
    var r = request(z("https://example.test/x"));
    r.http.timeout_ms = req.Bound.timeout_ms_max;
    _ = try req.startRequestValid(&r);
    r.http.timeout_ms = req.Bound.timeout_ms_max + 1;
    try std.testing.expectError(error.InvalidArg, req.startRequestValid(&r));
}

test "each optional header is checked against its own bound" {
    var r = request(z("https://example.test/x"));
    r.http.user_agent = z("ra8/1.0");
    r.http.referer = z("https://example.test/");
    r.http.if_none_match = z("\"abc\"");
    r.http.if_modified_since = z("Thu, 01 Oct 2026 00:00:00 GMT");
    _ = try req.startRequestValid(&r);

    var long: [req.Bound.http_date + 4]u8 = @splat('x');
    long[long.len - 1] = 0;
    r.http.if_modified_since = @ptrCast(&long);
    try std.testing.expectError(error.InvalidArg, req.startRequestValid(&r));
}

test "a header carrying a newline is refused" {
    var r = request(z("https://example.test/x"));
    r.http.user_agent = z("ra8\r\nX-Evil: 1");
    try std.testing.expectError(error.InvalidArg, req.startRequestValid(&r));
}

test "start fields carry every present header as a slice" {
    var r = request(z("https://example.test/x"));
    r.format = types.Format.rabook;
    r.http = .{
        .user_agent = z("ra8/1.0"),
        .referer = z("https://example.test/"),
        .if_none_match = z("\"abc\""),
        .if_modified_since = z("Thu, 01 Oct 2026 00:00:00 GMT"),
        .timeout_ms = 1500,
    };
    const length = try req.startRequestValid(&r);
    const fields = req.startFields(&r, length);
    try std.testing.expectEqualStrings("https://example.test/x", fields.url);
    try std.testing.expectEqual(@as(u8, types.Format.rabook), fields.format);
    try std.testing.expectEqualStrings("ra8/1.0", fields.user_agent);
    try std.testing.expectEqualStrings("https://example.test/", fields.referer);
    try std.testing.expectEqualStrings("\"abc\"", fields.if_none_match);
    try std.testing.expectEqualStrings("Thu, 01 Oct 2026 00:00:00 GMT", fields.if_modified_since);
    try std.testing.expectEqual(@as(u32, 1500), fields.timeout_ms);
}

test "an absent header is an empty slice, never a dangling pointer" {
    var r = request(z("https://example.test/x"));
    r.http.user_agent = z("ra8/1.0");
    const fields = req.startFields(&r, try req.startRequestValid(&r));
    try std.testing.expectEqualStrings("ra8/1.0", fields.user_agent);
    try std.testing.expectEqual(@as(usize, 0), fields.referer.len);
    try std.testing.expectEqual(@as(usize, 0), fields.if_none_match.len);
    try std.testing.expectEqual(@as(usize, 0), fields.if_modified_since.len);
}

test "the url is the length the check measured, not a second scan" {
    const r = request(z("https://example.test/abc"));
    const fields = req.startFields(&r, 20);
    try std.testing.expectEqualStrings("https://example.test", fields.url);
}
