//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The preconditions a media-download request is held to before it is sent.

const std = @import("std");
const implementation = @import("implementation");

const issue = implementation.mdl_issue;
const types = implementation.mdl_types;

fn active() types.Session {
    return .{ .job_id = 7, .max_chunk_bytes = 512, .active = true };
}

test "a cancel needs an active session" {
    try issue.cancelAllowed(&active());
}

test "an inactive session cannot be cancelled" {
    var session = active();
    session.active = false;
    try std.testing.expectError(error.InvalidState, issue.cancelAllowed(&session));
}

test "an active session with no job id cannot be cancelled" {
    var session = active();
    session.job_id = 0;
    try std.testing.expectError(error.InvalidState, issue.cancelAllowed(&session));
}

test "a default session is not correlatable" {
    const session = types.Session{};
    try std.testing.expectError(error.InvalidState, issue.cancelAllowed(&session));
}

test "next asks within the negotiated ceiling" {
    try issue.nextAllowed(&active(), 512);
    try issue.nextAllowed(&active(), 1);
}

test "next refuses a zero ask" {
    try std.testing.expectError(error.InvalidSize, issue.nextAllowed(&active(), 0));
}

test "next refuses more than the peer negotiated" {
    try std.testing.expectError(error.InvalidSize, issue.nextAllowed(&active(), 513));
}

test "next refuses more than the protocol ceiling even when the peer offered it" {
    var session = active();
    session.max_chunk_bytes = 4096;
    try issue.nextAllowed(&session, issue.Bound.chunk_data_max);
    try std.testing.expectError(
        error.InvalidSize,
        issue.nextAllowed(&session, issue.Bound.chunk_data_max + 1),
    );
}

test "next checks the session before the size" {
    var session = active();
    session.active = false;
    try std.testing.expectError(error.InvalidState, issue.nextAllowed(&session, 0));
}

test "the request buffer bound matches the protocol sum" {
    const sum: usize = 512 + 256 + 512 + 128 + 64 + 96;
    try std.testing.expectEqual(sum, issue.Bound.request_bytes_max);
}

test "the chunk ceiling matches the shared limit" {
    try std.testing.expectEqual(
        @as(u32, types.Limit.chunk_data_max),
        issue.Bound.chunk_data_max,
    );
}
