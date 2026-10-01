//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Service-side encoder for the media-download Accepted, Chunk and Cancelled
//! responses
//! (`proto/ra8_media_download.proto`), replacing the generated protobuf-c
//! pack. Byte-identical to the reference encoder, which the vectors pin.

const chunk_field = @import("mdl_chunk_decode.zig").Field;
const decode = @import("mdl_decode.zig");
const encode = @import("mdl_encode.zig");
const types = @import("mdl_types.zig");
const wire = @import("mdl_wire.zig");

pub const Error = wire.Error;

/// Encode an Accepted reply granting `job_id` and the largest chunk this
/// service sends. Returns the bytes written.
pub fn accepted(buf: []u8, job_id: u32, format: u8) Error![]const u8 {
    var w = wire.Writer.init(buf);
    try w.uint(decode.Accepted.protocol_version, encode.protocol_version);
    try w.uint(decode.Accepted.job_id, job_id);
    try w.uint(decode.Accepted.max_chunk_bytes, types.Limit.chunk_data_max);
    try w.uint(decode.Accepted.format, format);
    return w.written();
}

/// Encode a Cancelled acknowledgement with status zero, which proto3 leaves
/// off the wire. Returns the bytes written.
pub fn cancelled(buf: []u8, job_id: u32) Error![]const u8 {
    var w = wire.Writer.init(buf);
    try w.uint(decode.Cancelled.protocol_version, encode.protocol_version);
    try w.uint(decode.Cancelled.job_id, job_id);
    return w.written();
}

/// What a terminal Chunk adds: the digest and the final HTTP metadata.
pub const Terminal = struct {
    sha256: []const u8,
    http_status: i32,
    retry_after: []const u8 = "",
    etag: []const u8 = "",
    last_modified: []const u8 = "",
    content_type: []const u8 = "",
};

/// One Chunk the service sends. `terminal` set means the job is complete.
pub const Chunk = struct {
    job_id: u32,
    sequence: u32,
    offset: u64,
    data: []const u8,
    total: u64,
    terminal: ?Terminal = null,
};

/// Encode one Chunk, fields in number order. Status zero is left off the
/// wire, like every other proto3 default. Returns the bytes written.
pub fn chunk(buf: []u8, reply: Chunk) Error![]const u8 {
    var w = wire.Writer.init(buf);
    try w.uint(chunk_field.protocol_version, encode.protocol_version);
    try w.uint(chunk_field.job_id, reply.job_id);
    try w.uint(chunk_field.sequence, reply.sequence);
    try w.uint(chunk_field.offset, reply.offset);
    try w.bytes(chunk_field.data, reply.data);
    try w.uint(chunk_field.total_bytes, reply.total);
    const state = if (reply.terminal == null) types.State.downloading else types.State.complete;
    try w.uint(chunk_field.state, state);
    if (reply.terminal) |end| try terminal(&w, end);
    return w.written();
}

fn terminal(w: *wire.Writer, end: Terminal) Error!void {
    try w.bytes(chunk_field.sha256, end.sha256);
    try w.int32(chunk_field.http_status, end.http_status);
    try w.bytes(chunk_field.retry_after, end.retry_after);
    try w.bytes(chunk_field.etag, end.etag);
    try w.bytes(chunk_field.last_modified, end.last_modified);
    try w.bytes(chunk_field.content_type, end.content_type);
}
