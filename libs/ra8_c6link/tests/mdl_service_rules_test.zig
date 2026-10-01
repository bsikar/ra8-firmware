//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Contract tests for the value rules the portable media-download service
//! applies before anything with a side effect runs: header discipline, Start
//! request validity, terminal response metadata, the arena arithmetic, and the
//! packed-response capacity rule.

const std = @import("std");

const implementation = @import("implementation");
const rules = implementation.mdl_service_rules;

fn startRequest() rules.StartView {
    return .{
        .protocol_version = rules.Bound.protocol_version,
        .url = "https://example.test/book.rabook",
        .format = 0,
        .timeout_ms = 1000,
        .user_agent = "",
        .referer = "",
        .if_none_match = "",
        .if_modified_since = "",
    };
}

test "a header is bounded single-line text, and empty means absent" {
    try std.testing.expect(rules.fieldValid("", 8));
    try std.testing.expect(rules.fieldValid("abc", 8));
    try std.testing.expect(!rules.fieldValid(null, 8));
}

test "a header at or past its bound is refused, not truncated" {
    try std.testing.expect(rules.fieldValid("abc", 4));
    try std.testing.expect(!rules.fieldValid("abcd", 4));
    try std.testing.expect(!rules.fieldValid("abcde", 4));
}

test "a header carrying CR or LF is refused" {
    try std.testing.expect(!rules.fieldValid("a\rb", 16));
    try std.testing.expect(!rules.fieldValid("a\nb", 16));
    try std.testing.expect(!rules.fieldValid("trailing\r\n", 16));
}

test "a valid start request is accepted" {
    const request = startRequest();
    try std.testing.expect(rules.startValid(&request));
}

test "a start request must claim the protocol version this service speaks" {
    var request = startRequest();
    request.protocol_version = rules.Bound.protocol_version - 1;
    try std.testing.expect(!rules.startValid(&request));
    request.protocol_version = rules.Bound.protocol_version + 1;
    try std.testing.expect(!rules.startValid(&request));
}

test "a start url must be https with something after the scheme" {
    var request = startRequest();

    request.url = null;
    try std.testing.expect(!rules.startValid(&request));

    request.url = "";
    try std.testing.expect(!rules.startValid(&request));

    request.url = "http://example.test/x";
    try std.testing.expect(!rules.startValid(&request));

    request.url = "https://";
    try std.testing.expect(!rules.startValid(&request));

    request.url = "https://x";
    try std.testing.expect(rules.startValid(&request));
}

test "a start url at or past its bound is refused" {
    var long: [rules.Bound.url_max + 8]u8 = @splat('x');
    @memcpy(long[0..8], "https://");
    long[long.len - 1] = 0;

    var request = startRequest();
    request.url = @ptrCast(&long);
    try std.testing.expect(!rules.startValid(&request));

    var exact: [rules.Bound.url_max]u8 = @splat('x');
    @memcpy(exact[0..8], "https://");
    exact[rules.Bound.url_max - 1] = 0;
    request.url = @ptrCast(&exact);
    try std.testing.expect(rules.startValid(&request));

    var unterminated: [rules.Bound.url_max + 1]u8 = @splat('x');
    @memcpy(unterminated[0..8], "https://");
    unterminated[rules.Bound.url_max] = 0;
    request.url = @ptrCast(&unterminated);
    try std.testing.expect(!rules.startValid(&request));
}

test "a start format past the highest known format is refused" {
    var request = startRequest();
    request.format = rules.Bound.format_max;
    try std.testing.expect(rules.startValid(&request));
    request.format = rules.Bound.format_max + 1;
    try std.testing.expect(!rules.startValid(&request));
    request.format = 255;
    try std.testing.expect(!rules.startValid(&request));
}

test "a start timeout past the cap is refused" {
    var request = startRequest();
    request.timeout_ms = rules.Bound.timeout_ms_max;
    try std.testing.expect(rules.startValid(&request));
    request.timeout_ms = rules.Bound.timeout_ms_max + 1;
    try std.testing.expect(!rules.startValid(&request));
}

test "each start header is bounded by its own cap" {
    var request = startRequest();
    request.user_agent = null;
    try std.testing.expect(!rules.startValid(&request));

    request = startRequest();
    request.referer = "a\r\nb";
    try std.testing.expect(!rules.startValid(&request));

    request = startRequest();
    var date: [rules.Bound.http_date_max + 1]u8 = @splat('x');
    date[date.len - 1] = 0;
    request.if_modified_since = @ptrCast(&date);
    try std.testing.expect(!rules.startValid(&request));

    request = startRequest();
    var etag: [rules.Bound.etag_max]u8 = @splat('x');
    etag[rules.Bound.etag_max - 2] = 0;
    request.if_none_match = @ptrCast(&etag);
    try std.testing.expect(rules.startValid(&request));
}

test "terminal response metadata needs an http-shaped status" {
    var response = rules.ResponseView{ .status = 200 };
    try std.testing.expect(rules.responseValid(&response));

    for ([_]i32{ 0, 99, 600, -1 }) |status| {
        response.status = status;
        try std.testing.expect(!rules.responseValid(&response));
    }
    response.status = rules.Bound.status_min;
    try std.testing.expect(rules.responseValid(&response));
    response.status = rules.Bound.status_max;
    try std.testing.expect(rules.responseValid(&response));
}

test "every selected response header is bounded independently" {
    var response = rules.ResponseView{ .status = 200 };
    @memcpy(response.etag[0..5], "\"abc\"");
    @memcpy(response.content_type[0..15], "application/zip");
    try std.testing.expect(rules.responseValid(&response));

    response.last_modified[0] = '\n';
    try std.testing.expect(!rules.responseValid(&response));
}

test "an unterminated response header is refused" {
    var response = rules.ResponseView{ .status = 200 };
    response.retry_after = @splat('x');
    try std.testing.expect(!rules.responseValid(&response));
}

test "an arena request fits only when its rounded size does" {
    const cap: usize = 64;
    try std.testing.expect(rules.allocationFits(0, 64, cap));
    try std.testing.expect(!rules.allocationFits(0, 65, cap));
    try std.testing.expect(rules.allocationFits(56, 8, cap));
    try std.testing.expect(!rules.allocationFits(56, 9, cap));
    try std.testing.expect(rules.allocationFits(0, 0, cap));
}

test "rounding is what decides a near-capacity request" {
    const cap: usize = 64;
    try std.testing.expect(rules.allocationFits(0, 57, cap));
    try std.testing.expect(!rules.allocationFits(8, 57, cap));
    try std.testing.expectEqual(@as(usize, 64), rules.alignedSize(57));
}

test "a length that would wrap the rounding is refused" {
    try std.testing.expect(!rules.allocationFits(
        0,
        std.math.maxInt(usize),
        std.math.maxInt(usize),
    ));
    try std.testing.expect(!rules.allocationFits(
        0,
        std.math.maxInt(usize) - 6,
        std.math.maxInt(usize),
    ));
}

test "an aligned size is a multiple of the published alignment" {
    for ([_]usize{ 0, 1, 7, 8, 9, 63, 64, 65 }) |len| {
        const size = rules.alignedSize(len);
        try std.testing.expect(size >= len);
        try std.testing.expectEqual(@as(usize, 0), size % rules.Bound.decode_align);
        try std.testing.expect(size - len < rules.Bound.decode_align);
    }
}

test "a packed response fits only when it is non-empty and within capacity" {
    try std.testing.expect(rules.responseSizeOk(1, 1));
    try std.testing.expect(rules.responseSizeOk(32, 64));
    try std.testing.expect(!rules.responseSizeOk(0, 64));
    try std.testing.expect(!rules.responseSizeOk(65, 64));
    try std.testing.expect(!rules.responseSizeOk(1, 0));
}
