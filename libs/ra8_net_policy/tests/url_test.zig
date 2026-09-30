//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The scheme allowlist, the authority copy, and the path copy.

const std = @import("std");
const testing = std.testing;

const url_policy = @import("url_policy");

const ok: u16 = 0;
const no_mem: u16 = 0x102;
const not_found: u16 = 0x106;

fn host(buf: []u8, text: []const u8) u16 {
    return url_policy.copyHost(text, buf);
}

fn path(buf: []u8, text: []const u8) u16 {
    return url_policy.copyPath(text, buf);
}

fn asText(buf: []const u8) []const u8 {
    return buf[0 .. std.mem.indexOfScalar(u8, buf, 0) orelse buf.len];
}

test "only http and https are allowed, in any case" {
    try testing.expect(url_policy.schemeAllowed("http://a"));
    try testing.expect(url_policy.schemeAllowed("https://a"));
    try testing.expect(url_policy.schemeAllowed("HTTP://a"));
    try testing.expect(url_policy.schemeAllowed("HtTpS://a"));
    try testing.expect(!url_policy.schemeAllowed(""));
    try testing.expect(!url_policy.schemeAllowed("ftp://a"));
    try testing.expect(!url_policy.schemeAllowed("file:///etc/passwd"));
    try testing.expect(!url_policy.schemeAllowed("http:/a"));
    try testing.expect(!url_policy.schemeAllowed("http"));
}

test "the authority comes back lower-cased" {
    var buf: [64]u8 = undefined;
    try testing.expectEqual(ok, host(&buf, "http://Example.COM/Path"));
    try testing.expectEqualStrings("example.com", asText(&buf));
}

test "the port is part of the origin and is kept" {
    var buf: [64]u8 = undefined;
    try testing.expectEqual(ok, host(&buf, "https://example.com:8443/x"));
    try testing.expectEqualStrings("example.com:8443", asText(&buf));
}

test "userinfo is dropped" {
    var buf: [64]u8 = undefined;
    try testing.expectEqual(ok, host(&buf, "http://user:pw@example.com/x"));
    try testing.expectEqualStrings("example.com", asText(&buf));
}

test "an at sign after the path start is not userinfo" {
    var buf: [64]u8 = undefined;
    try testing.expectEqual(ok, host(&buf, "http://example.com/mail@host"));
    try testing.expectEqualStrings("example.com", asText(&buf));
}

test "the authority ends at the first path, query or fragment byte" {
    var buf: [64]u8 = undefined;
    try testing.expectEqual(ok, host(&buf, "http://example.com?q=1"));
    try testing.expectEqualStrings("example.com", asText(&buf));
    try testing.expectEqual(ok, host(&buf, "http://example.com#frag"));
    try testing.expectEqualStrings("example.com", asText(&buf));
}

test "no separator and an empty authority are both not found" {
    var buf: [64]u8 = undefined;
    try testing.expectEqual(not_found, host(&buf, "example.com/x"));
    try testing.expectEqual(not_found, host(&buf, "http:///just/a/path"));
}

test "a short buffer refuses and leaves an empty string behind" {
    var buf: [4]u8 = undefined;
    try testing.expectEqual(no_mem, host(&buf, "http://example.com/"));
    try testing.expectEqualStrings("", asText(&buf));
}

test "the authority fits exactly when the buffer has room for the NUL" {
    var buf: [12]u8 = undefined;
    try testing.expectEqual(ok, host(&buf, "http://example.com/"));
    try testing.expectEqualStrings("example.com", asText(&buf));
    var tight: [11]u8 = undefined;
    try testing.expectEqual(no_mem, host(&tight, "http://example.com/"));
}

test "the path comes back with query and fragment cut" {
    var buf: [64]u8 = undefined;
    try testing.expectEqual(ok, path(&buf, "http://example.com/a/b?q=1"));
    try testing.expectEqualStrings("/a/b", asText(&buf));
    try testing.expectEqual(ok, path(&buf, "http://example.com/a/b#frag"));
    try testing.expectEqualStrings("/a/b", asText(&buf));
}

test "a URL with no path yields a bare slash" {
    var buf: [64]u8 = undefined;
    try testing.expectEqual(ok, path(&buf, "http://example.com"));
    try testing.expectEqualStrings("/", asText(&buf));
    try testing.expectEqual(ok, path(&buf, "http://example.com?q=1"));
    try testing.expectEqualStrings("/", asText(&buf));
}

test "the bare slash still needs room for its NUL" {
    var buf: [1]u8 = undefined;
    try testing.expectEqual(no_mem, path(&buf, "http://example.com"));
}

test "the path is case-preserving, unlike the authority" {
    var buf: [64]u8 = undefined;
    try testing.expectEqual(ok, path(&buf, "http://EXAMPLE.com/CaseKept"));
    try testing.expectEqualStrings("/CaseKept", asText(&buf));
}

test "no separator means no path" {
    var buf: [64]u8 = undefined;
    try testing.expectEqual(not_found, path(&buf, "example.com/x"));
}

test "a short buffer refuses the path and empties the output" {
    var buf: [4]u8 = undefined;
    try testing.expectEqual(no_mem, path(&buf, "http://example.com/long/path"));
    try testing.expectEqualStrings("", asText(&buf));
}
