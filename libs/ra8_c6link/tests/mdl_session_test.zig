//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Tests for the media-download response correlation rules.

const std = @import("std");
const session_rules = @import("implementation").mdl_session;
const types = @import("implementation").mdl_types;

fn accepted() session_rules.AcceptedView {
    return .{
        .protocol_version = session_rules.Bound.protocol_version,
        .job_id = 7,
        .max_chunk_bytes = 512,
        .format = 2,
        .unknown_fields = 0,
    };
}

fn active() types.Session {
    return .{
        .job_id = 7,
        .next_sequence = 3,
        .next_offset = 1536,
        .max_chunk_bytes = 512,
        .format = 2,
        .active = true,
    };
}

fn chunkKey() session_rules.ChunkKeyView {
    return .{
        .protocol_version = session_rules.Bound.protocol_version,
        .job_id = 7,
        .sequence = 3,
        .offset = 1536,
        .data_len = 512,
        .data_present = true,
        .unknown_fields = 0,
    };
}

fn cancelled() session_rules.CancelledView {
    return .{
        .protocol_version = session_rules.Bound.protocol_version,
        .job_id = 7,
        .status = 0,
        .unknown_fields = 0,
    };
}

test "a well-formed accepted response opens the job" {
    try std.testing.expect(session_rules.acceptedValid(&accepted(), 2));
}

test "an accepted response for another format is refused" {
    try std.testing.expect(!session_rules.acceptedValid(&accepted(), 5));
}

test "an accepted response on the wrong protocol version is refused" {
    var view = accepted();
    view.protocol_version = session_rules.Bound.protocol_version + 1;
    try std.testing.expect(!session_rules.acceptedValid(&view, 2));
}

test "an accepted response with job id zero is refused" {
    var view = accepted();
    view.job_id = 0;
    try std.testing.expect(!session_rules.acceptedValid(&view, 2));
}

test "an accepted response offering no chunk bytes is refused" {
    var view = accepted();
    view.max_chunk_bytes = 0;
    try std.testing.expect(!session_rules.acceptedValid(&view, 2));
}

test "an accepted response at the chunk ceiling is allowed" {
    var view = accepted();
    view.max_chunk_bytes = session_rules.Bound.chunk_data_max;
    try std.testing.expect(session_rules.acceptedValid(&view, 2));
}

test "an accepted response above the chunk ceiling is refused" {
    var view = accepted();
    view.max_chunk_bytes = session_rules.Bound.chunk_data_max + 1;
    try std.testing.expect(!session_rules.acceptedValid(&view, 2));
}

test "an accepted response carrying unknown fields is refused" {
    var view = accepted();
    view.unknown_fields = 1;
    try std.testing.expect(!session_rules.acceptedValid(&view, 2));
}

test "activation opens the session at the start of the stream" {
    var sess: types.Session = .{};
    session_rules.activate(&accepted(), &sess, 2);
    try std.testing.expect(sess.active);
    try std.testing.expectEqual(@as(u32, 7), sess.job_id);
    try std.testing.expectEqual(@as(u32, 0), sess.next_sequence);
    try std.testing.expectEqual(@as(u64, 0), sess.next_offset);
    try std.testing.expectEqual(@as(u32, 512), sess.max_chunk_bytes);
    try std.testing.expectEqual(@as(u8, 2), sess.format);
}

test "activation overwrites whatever the session held before" {
    var sess = active();
    session_rules.activate(&accepted(), &sess, 2);
    try std.testing.expectEqual(@as(u32, 0), sess.next_sequence);
    try std.testing.expectEqual(@as(u64, 0), sess.next_offset);
}

test "the awaited chunk correlates" {
    const sess = active();
    try std.testing.expect(session_rules.chunkCorrelates(&chunkKey(), &sess, 512));
}

test "a chunk for another job is refused" {
    const sess = active();
    var key = chunkKey();
    key.job_id = 8;
    try std.testing.expect(!session_rules.chunkCorrelates(&key, &sess, 512));
}

test "a replayed sequence is refused" {
    const sess = active();
    var key = chunkKey();
    key.sequence = 2;
    try std.testing.expect(!session_rules.chunkCorrelates(&key, &sess, 512));
}

test "a chunk at the wrong offset is refused" {
    const sess = active();
    var key = chunkKey();
    key.offset = 1024;
    try std.testing.expect(!session_rules.chunkCorrelates(&key, &sess, 512));
}

test "a chunk longer than the caller asked for is refused" {
    const sess = active();
    var key = chunkKey();
    key.data_len = 513;
    try std.testing.expect(!session_rules.chunkCorrelates(&key, &sess, 512));
}

test "a chunk above the protocol ceiling is refused even when requested" {
    const sess = active();
    var key = chunkKey();
    key.data_len = session_rules.Bound.chunk_data_max + 1;
    try std.testing.expect(!session_rules.chunkCorrelates(&key, &sess, 4096));
}

test "an empty chunk needs no body pointer" {
    const sess = active();
    var key = chunkKey();
    key.data_len = 0;
    key.data_present = false;
    try std.testing.expect(session_rules.chunkCorrelates(&key, &sess, 512));
}

test "a non-empty chunk with no body pointer is refused" {
    const sess = active();
    var key = chunkKey();
    key.data_present = false;
    try std.testing.expect(!session_rules.chunkCorrelates(&key, &sess, 512));
}

test "a chunk on the wrong protocol version is refused" {
    const sess = active();
    var key = chunkKey();
    key.protocol_version = 0;
    try std.testing.expect(!session_rules.chunkCorrelates(&key, &sess, 512));
}

test "a chunk carrying unknown fields is refused" {
    const sess = active();
    var key = chunkKey();
    key.unknown_fields = 2;
    try std.testing.expect(!session_rules.chunkCorrelates(&key, &sess, 512));
}

test "a matching cancellation is accepted" {
    const sess = active();
    try std.testing.expect(session_rules.cancelledValid(&cancelled(), &sess));
}

test "a cancellation for another job is refused" {
    const sess = active();
    var view = cancelled();
    view.job_id = 9;
    try std.testing.expect(!session_rules.cancelledValid(&view, &sess));
}

test "a cancellation reporting a remote failure is refused" {
    const sess = active();
    var view = cancelled();
    view.status = -1;
    try std.testing.expect(!session_rules.cancelledValid(&view, &sess));
}

test "a cancellation on the wrong protocol version is refused" {
    const sess = active();
    var view = cancelled();
    view.protocol_version = 2;
    try std.testing.expect(!session_rules.cancelledValid(&view, &sess));
}

test "deactivation closes the session and keeps its correlation state" {
    var sess = active();
    session_rules.deactivate(&sess);
    try std.testing.expect(!sess.active);
    try std.testing.expectEqual(@as(u32, 7), sess.job_id);
    try std.testing.expectEqual(@as(u32, 3), sess.next_sequence);
}
