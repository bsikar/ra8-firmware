//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Contract tests for the media-download chunk rules: the terminal HTTP
//! metadata, the state-specific field combinations, and the one copy that
//! applies them to caller storage.

const std = @import("std");

const implementation = @import("implementation");
const chunk = implementation.mdl_chunk;
const types = implementation.mdl_types;

const empty: []const u8 = "";

fn downloading(body: []const u8) chunk.View {
    return .{
        .job_id = 7,
        .sequence = 0,
        .offset = 0,
        .total_bytes = 0,
        .state = types.State.downloading,
        .data = body,
        .retry_after = empty,
        .etag = empty,
        .last_modified = empty,
        .content_type = empty,
    };
}

fn complete(digest: *const [32]u8) chunk.View {
    return .{
        .job_id = 7,
        .state = types.State.complete,
        .sha256 = digest,
        .http_status = 200,
        .retry_after = empty,
        .etag = empty,
        .last_modified = empty,
        .content_type = empty,
    };
}

test "a non-terminal chunk must carry no http metadata" {
    var view = downloading("abc");
    try std.testing.expect(chunk.httpResponseValid(&view));

    view.http_status = 200;
    try std.testing.expect(!chunk.httpResponseValid(&view));

    view = downloading("abc");
    view.etag = "\"x\"";
    try std.testing.expect(!chunk.httpResponseValid(&view));
}

test "a null header on a non-terminal chunk is refused, not treated as absent" {
    var view = downloading("abc");
    view.content_type = null;
    try std.testing.expect(!chunk.httpResponseValid(&view));
}

test "a complete chunk needs a real status in range" {
    var digest: [32]u8 = @splat(0xAB);
    var view = complete(&digest);
    try std.testing.expect(chunk.httpResponseValid(&view));

    for ([_]i32{ 0, 99, 600 }) |status| {
        view.http_status = status;
        try std.testing.expect(!chunk.httpResponseValid(&view));
    }
    view.http_status = 100;
    try std.testing.expect(chunk.httpResponseValid(&view));
    view.http_status = 599;
    try std.testing.expect(chunk.httpResponseValid(&view));
}

test "a complete chunk's headers are bounded and single-line" {
    var digest: [32]u8 = @splat(0xAB);
    var view = complete(&digest);
    view.etag = "\"abc\"";
    view.content_type = "application/zip";
    try std.testing.expect(chunk.httpResponseValid(&view));

    view.content_type = "application/zip\r\nX-Evil: 1";
    try std.testing.expect(!chunk.httpResponseValid(&view));
}

test "downloading carries data and no digest" {
    var view = downloading("abc");
    try std.testing.expect(chunk.semanticsValid(&view));

    view.data = null;
    try std.testing.expect(!chunk.semanticsValid(&view));

    view = downloading("abc");
    view.status = 5;
    try std.testing.expect(!chunk.semanticsValid(&view));

    var digest: [32]u8 = @splat(0);
    view = downloading("abc");
    view.sha256 = &digest;
    try std.testing.expect(!chunk.semanticsValid(&view));
}

test "complete carries a digest and no data" {
    var digest: [32]u8 = @splat(0xAB);
    var view = complete(&digest);
    try std.testing.expect(chunk.semanticsValid(&view));

    view.sha256 = digest[0..31];
    try std.testing.expect(!chunk.semanticsValid(&view));

    view = complete(&digest);
    view.sha256 = null;
    try std.testing.expect(!chunk.semanticsValid(&view));

    view = complete(&digest);
    view.data = "x";
    try std.testing.expect(!chunk.semanticsValid(&view));
}

test "complete must land exactly on an advertised total" {
    var digest: [32]u8 = @splat(0xAB);
    var view = complete(&digest);
    view.offset = 100;
    view.total_bytes = 100;
    try std.testing.expect(chunk.semanticsValid(&view));

    view.total_bytes = 101;
    try std.testing.expect(!chunk.semanticsValid(&view));
}

test "cancelled carries neither data nor digest" {
    var view = downloading("");
    view.state = types.State.cancelled;
    view.data = null;
    try std.testing.expect(chunk.semanticsValid(&view));

    view.status = 1;
    try std.testing.expect(!chunk.semanticsValid(&view));
}

test "failed carries a real status that fits the public error type" {
    var view = downloading("");
    view.state = types.State.failed;
    view.data = null;
    view.status = 0x101;
    try std.testing.expect(chunk.semanticsValid(&view));

    view.status = 0;
    try std.testing.expect(!chunk.semanticsValid(&view));

    view.status = std.math.maxInt(u16);
    try std.testing.expect(chunk.semanticsValid(&view));
    view.status = std.math.maxInt(u16) + 1;
    try std.testing.expect(!chunk.semanticsValid(&view));
}

test "an unknown state is refused" {
    var view = downloading("abc");
    view.state = types.State.accepted;
    try std.testing.expect(!chunk.semanticsValid(&view));
    view.state = 99;
    try std.testing.expect(!chunk.semanticsValid(&view));
}

test "data may not run past an advertised total" {
    var view = downloading("abcd");
    view.offset = 8;
    view.total_bytes = 10;
    try std.testing.expect(!chunk.semanticsValid(&view));

    view.total_bytes = 12;
    try std.testing.expect(chunk.semanticsValid(&view));
}

test "an end offset that would overflow is refused before anything else" {
    var view = downloading("abcd");
    view.offset = std.math.maxInt(u64) - 2;
    try std.testing.expect(!chunk.semanticsValid(&view));
}

test "accepting a data chunk copies the body and advances the session" {
    var session = types.Session{ .job_id = 7, .active = true, .next_offset = 4, .next_sequence = 1 };
    var out: types.Chunk = .{};
    var view = downloading("hello");
    view.offset = 4;
    view.sequence = 1;

    try std.testing.expectEqual(@as(u16, 0), chunk.accept(&view, &session, &out));
    try std.testing.expectEqualStrings("hello", out.data[0..out.data_len]);
    try std.testing.expectEqual(@as(u64, 9), session.next_offset);
    try std.testing.expectEqual(@as(u32, 2), session.next_sequence);
    try std.testing.expect(session.active);
    try std.testing.expect(!out.has_sha256);
}

test "accepting a complete chunk copies the digest, headers, and retires the session" {
    var digest: [32]u8 = @splat(0xAB);
    var session = types.Session{ .job_id = 7, .active = true };
    var out: types.Chunk = .{};
    var view = complete(&digest);
    view.etag = "\"abc\"";
    view.content_type = "application/zip";

    try std.testing.expectEqual(@as(u16, 0), chunk.accept(&view, &session, &out));
    try std.testing.expect(out.has_sha256);
    try std.testing.expectEqualSlices(u8, &digest, &out.sha256);
    try std.testing.expectEqual(@as(i32, 200), out.response.status);
    try std.testing.expectEqualStrings("\"abc\"", std.mem.sliceTo(&out.response.etag, 0));
    try std.testing.expectEqualStrings(
        "application/zip",
        std.mem.sliceTo(&out.response.content_type, 0),
    );
    try std.testing.expect(!session.active);
}

test "a failed chunk returns the remote status and retires the session" {
    var session = types.Session{ .job_id = 7, .active = true };
    var out: types.Chunk = .{};
    var view = downloading("");
    view.state = types.State.failed;
    view.data = null;
    view.status = 0x108;

    try std.testing.expectEqual(@as(u16, 0x108), chunk.accept(&view, &session, &out));
    try std.testing.expectEqual(@as(u16, 0x108), out.status);
    try std.testing.expect(!session.active);
}

test "a cancelled chunk retires the session without a status" {
    var session = types.Session{ .job_id = 7, .active = true };
    var out: types.Chunk = .{};
    var view = downloading("");
    view.state = types.State.cancelled;
    view.data = null;

    try std.testing.expectEqual(@as(u16, 0), chunk.accept(&view, &session, &out));
    try std.testing.expect(!session.active);
}

test "a non-terminal chunk leaves the response storage untouched" {
    var session = types.Session{ .job_id = 7, .active = true };
    var out: types.Chunk = .{};
    out.response.status = 0;
    var view = downloading("abc");

    _ = chunk.accept(&view, &session, &out);
    try std.testing.expectEqual(@as(i32, 0), out.response.status);
    try std.testing.expectEqual(@as(u8, 0), out.response.etag[0]);
}

test "a header copy terminates and does not run past its storage" {
    var digest: [32]u8 = @splat(0);
    var session = types.Session{ .job_id = 7, .active = true };
    var out: types.Chunk = .{};
    var view = complete(&digest);

    const long: [types.Limit.etag_max + 16]u8 = @splat('x');
    view.etag = &long;

    _ = chunk.accept(&view, &session, &out);
    try std.testing.expectEqual(
        @as(usize, types.Limit.etag_max - 1),
        std.mem.sliceTo(&out.response.etag, 0).len,
    );
    try std.testing.expectEqual(@as(u8, 0), out.response.etag[types.Limit.etag_max - 1]);
}

test "a header must fit its terminated storage" {
    const cap = types.Limit.etag_max;
    const fits: [cap - 1]u8 = @splat('x');
    const full: [cap]u8 = @splat('x');
    try std.testing.expect(chunk.fieldValid(&fits, cap));
    try std.testing.expect(!chunk.fieldValid(&full, cap));
}

test "an unset header is valid, and a header with CR, LF or NUL is not" {
    try std.testing.expect(chunk.fieldValid(null, 8));
    try std.testing.expect(chunk.fieldValid("", 8));
    for ([_][]const u8{ "a\rb", "a\nb", "a\x00b" }) |bad| {
        try std.testing.expect(!chunk.fieldValid(bad, 8));
    }
}
