//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The media-download extractor admission rules.

const std = @import("std");
const implementation = @import("implementation");

const take = implementation.mdl_take;
const envelope = implementation.mdl_envelope;
const mdl_chunk = implementation.mdl_chunk;
const mdl_session = implementation.mdl_session;
const types = implementation.mdl_types;

const accepted_kind: u8 = @intFromEnum(envelope.Kind.accepted);
const chunk_kind: u8 = @intFromEnum(envelope.Kind.chunk);
const cancelled_kind: u8 = @intFromEnum(envelope.Kind.cancelled);

fn reply(operation: u32) envelope.ResponseView {
    return .{
        .custom_msg_id = operation,
        .operation = operation,
        .body_len = 8,
        .body_present = true,
    };
}

test "a start reply runs the accepted extractor" {
    const view = reply(envelope.Bound.rpc_start);
    try std.testing.expectEqual(envelope.Kind.accepted, take.selected(&view, accepted_kind).?);
}

test "a pull reply runs the chunk extractor" {
    const view = reply(envelope.Bound.rpc_next);
    try std.testing.expectEqual(envelope.Kind.chunk, take.selected(&view, chunk_kind).?);
}

test "a cancel reply runs the cancelled extractor" {
    const view = reply(envelope.Bound.rpc_cancel);
    try std.testing.expectEqual(envelope.Kind.cancelled, take.selected(&view, cancelled_kind).?);
}

test "a reply for another operation runs no extractor" {
    const view = reply(envelope.Bound.rpc_next);
    try std.testing.expectEqual(@as(?take.Kind, null), take.selected(&view, accepted_kind));
}

test "an envelope the rules refuse runs no extractor" {
    var view = reply(envelope.Bound.rpc_start);
    view.custom_msg_id = envelope.Bound.rpc_next;
    try std.testing.expectEqual(@as(?take.Kind, null), take.selected(&view, accepted_kind));
}

test "an empty body runs no extractor even for the expected kind" {
    var view = reply(envelope.Bound.rpc_next);
    view.body_len = 0;
    try std.testing.expectEqual(@as(?take.Kind, null), take.selected(&view, chunk_kind));
}

test "an unknown operation runs no extractor" {
    const view = reply(0x4D44_0399);
    try std.testing.expectEqual(@as(?take.Kind, null), take.selected(&view, chunk_kind));
    try std.testing.expectEqual(@as(?take.Kind, null), take.selected(&view, accepted_kind));
}

test "an expected kind outside the enum runs no extractor" {
    const view = reply(envelope.Bound.rpc_start);
    try std.testing.expectEqual(@as(?take.Kind, null), take.selected(&view, 0));
    try std.testing.expectEqual(@as(?take.Kind, null), take.selected(&view, 9));
}

fn session() types.Session {
    return .{
        .job_id = 7,
        .next_sequence = 3,
        .next_offset = 2048,
        .max_chunk_bytes = 1024,
        .format = 1,
        .active = true,
    };
}

fn key(current: *const types.Session, data_len: u32) mdl_session.ChunkKeyView {
    return .{
        .protocol_version = 3,
        .job_id = current.job_id,
        .sequence = current.next_sequence,
        .offset = current.next_offset,
        .data_len = data_len,
        .data_present = data_len != 0,
        .unknown_fields = 0,
    };
}

const empty: []const u8 = "";

fn body(current: *const types.Session, bytes: []const u8) mdl_chunk.View {
    return .{
        .job_id = current.job_id,
        .sequence = current.next_sequence,
        .offset = current.next_offset,
        .total_bytes = 8192,
        .state = types.State.downloading,
        .status = 0,
        .data = bytes,
        .retry_after = empty,
        .etag = empty,
        .last_modified = empty,
        .content_type = empty,
    };
}

test "a correlated coherent chunk is admissible" {
    const bytes = [_]u8{0xAB} ** 64;
    const current = session();
    try std.testing.expect(take.chunkAdmissible(
        &key(&current, bytes.len),
        &body(&current, bytes[0..]),
        &current,
        512,
    ));
}

test "a chunk for another sequence is refused" {
    const bytes = [_]u8{0xAB} ** 64;
    const current = session();
    var wrong = key(&current, bytes.len);
    wrong.sequence = current.next_sequence + 1;
    try std.testing.expect(!take.chunkAdmissible(&wrong, &body(&current, bytes[0..]), &current, 512));
}

test "a chunk longer than the pull asked for is refused" {
    const bytes = [_]u8{0xAB} ** 64;
    const current = session();
    try std.testing.expect(!take.chunkAdmissible(
        &key(&current, bytes.len),
        &body(&current, bytes[0..]),
        &current,
        32,
    ));
}

test "a correlated chunk with incoherent state is refused" {
    const bytes = [_]u8{0xAB} ** 64;
    const current = session();
    var incoherent = body(&current, bytes[0..]);
    incoherent.state = types.State.complete;
    try std.testing.expect(!take.chunkAdmissible(&key(&current, bytes.len), &incoherent, &current, 512));
}

test "both halves have to hold, neither alone admits" {
    const bytes = [_]u8{0xAB} ** 64;
    const current = session();
    var wrong = key(&current, bytes.len);
    wrong.job_id = current.job_id + 1;
    var incoherent = body(&current, bytes[0..]);
    incoherent.state = types.State.complete;
    try std.testing.expect(!take.chunkAdmissible(&wrong, &body(&current, bytes[0..]), &current, 512));
    try std.testing.expect(!take.chunkAdmissible(&key(&current, bytes.len), &incoherent, &current, 512));
    try std.testing.expect(!take.chunkAdmissible(&wrong, &incoherent, &current, 512));
}
