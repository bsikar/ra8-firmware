//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The media-download chunk rules: what a decoded response has to look like
//! before any of it reaches caller memory, and the one copy that applies it.
//!
//! The decoder (`mdl_chunk_decode.zig`) fills a `View`: the decoded field
//! values, flat, borrowing the response buffer. Every rule about those values
//! lives here.

const std = @import("std");

const types = @import("mdl_types.zig");

/// Bounds the C6 service's HTTP metadata has to respect.
pub const Bound = struct {
    pub const status_min: i32 = 100;
    pub const status_max: i32 = 599;
    pub const retry_after: usize = types.Limit.retry_after_max;
    pub const etag: usize = types.Limit.etag_max;
    pub const http_date: usize = types.Limit.http_date_max;
    pub const content_type: usize = types.Limit.content_type_max;
};

/// One decoded chunk response, as flat values borrowing the response buffer.
///
/// A null span was never decoded. The decoder leaves an absent body or digest
/// null and an absent header empty, the shapes protobuf-c produced.
pub const View = struct {
    job_id: u32 = 0,
    sequence: u32 = 0,
    offset: u64 = 0,
    total_bytes: u64 = 0,
    state: u8 = 0,
    status: i32 = 0,
    data: ?[]const u8 = null,
    sha256: ?[]const u8 = null,
    http_status: i32 = 0,
    retry_after: ?[]const u8 = null,
    etag: ?[]const u8 = null,
    last_modified: ?[]const u8 = null,
    content_type: ?[]const u8 = null,
};

/// The length of an optional span, zero when it is null.
pub fn len(span: ?[]const u8) usize {
    return if (span) |bytes| bytes.len else 0;
}

/// Was the header decoded, and empty?
fn blank(text: ?[]const u8) bool {
    const bytes = text orelse return false;
    return bytes.len == 0;
}

/// One optional header: unset, or shorter than its terminated storage and free
/// of CR, LF and NUL. A NUL would silently cut the copy short, so it is
/// refused rather than inherited from the old C-string view.
pub fn fieldValid(text: ?[]const u8, cap: usize) bool {
    const bytes = text orelse return true;
    if (bytes.len >= cap) return false;
    return std.mem.indexOfAny(u8, bytes, "\r\n\x00") == null;
}

/// Is every selected header absent, so a non-terminal chunk carries no metadata?
fn headersEmpty(view: *const View) bool {
    return blank(view.retry_after) and blank(view.etag) and
        blank(view.last_modified) and blank(view.content_type);
}

/// Does the terminal HTTP metadata match the chunk's state and its bounds?
///
/// Only a COMPLETE chunk carries a real status and headers. Anything else has
/// to arrive with a zero status and four empty strings, so a mid-transfer
/// response cannot smuggle header bytes into caller storage.
pub fn httpResponseValid(view: *const View) bool {
    if (view.state != types.State.complete) {
        return view.http_status == 0 and headersEmpty(view);
    }
    return view.http_status >= Bound.status_min and
        view.http_status <= Bound.status_max and
        fieldValid(view.retry_after, Bound.retry_after) and
        fieldValid(view.etag, Bound.etag) and
        fieldValid(view.last_modified, Bound.http_date) and
        fieldValid(view.content_type, Bound.content_type);
}

/// Are the state-specific fields of one correlated chunk coherent?
///
/// Each state admits exactly one shape: DOWNLOADING carries data and no
/// digest, COMPLETE carries a 32-byte digest and no data, CANCELLED carries
/// neither, FAILED carries a real status and neither. The totals are checked first so
/// a later bounded copy cannot be handed an end offset that overflowed.
pub fn semanticsValid(view: *const View) bool {
    const data_len = len(view.data);
    const digest_len = len(view.sha256);
    if (data_len > std.math.maxInt(u64) - view.offset) return false;
    const end = view.offset + data_len;
    if (view.total_bytes != 0 and end > view.total_bytes) return false;
    if (!httpResponseValid(view)) return false;

    return switch (view.state) {
        types.State.downloading => view.status == 0 and
            data_len != 0 and digest_len == 0,
        types.State.complete => view.status == 0 and
            data_len == 0 and
            digest_len == types.Limit.sha256_bytes and
            (view.total_bytes == 0 or view.total_bytes == end),
        types.State.cancelled => view.status == 0 and
            data_len == 0 and digest_len == 0,
        types.State.failed => view.status > 0 and
            view.status <= std.math.maxInt(u16) and
            data_len == 0 and digest_len == 0,
        else => false,
    };
}

/// Copy one header into fixed response storage, always terminated.
///
/// `httpResponseValid` already rejects anything longer than the storage, so a
/// source that reaches the cap here means validation was skipped. Truncate to
/// the last byte rather than run past the destination.
fn copyField(destination: []u8, source: ?[]const u8) void {
    const bytes = source orelse return;
    const length = @min(bytes.len, destination.len - 1);
    @memcpy(destination[0..length], bytes[0..length]);
    destination[length] = 0;
}

/// Copy one validated chunk into caller storage and advance its session.
///
/// Correlation and semantics were checked before this runs, so every copy
/// here is size-safe. The session advances by exactly the decoded data length
/// and a terminal state retires it.
///
/// Returns the remote's own status on FAILED, and ok otherwise.
pub fn accept(view: *const View, session: *types.Session, chunk: *types.Chunk) u16 {
    const data_len = len(view.data);
    chunk.* = .{
        .job_id = view.job_id,
        .sequence = view.sequence,
        .offset = view.offset,
        .total_bytes = view.total_bytes,
        .state = view.state,
        .status = @truncate(@as(u32, @bitCast(view.status))),
        .data_len = @intCast(data_len),
        .has_sha256 = len(view.sha256) == types.Limit.sha256_bytes,
    };
    if (view.data) |body| @memcpy(chunk.data[0..body.len], body);
    if (chunk.has_sha256) @memcpy(&chunk.sha256, view.sha256.?);
    if (view.state == types.State.complete) {
        chunk.response.status = view.http_status;
        copyField(&chunk.response.retry_after, view.retry_after);
        copyField(&chunk.response.etag, view.etag);
        copyField(&chunk.response.last_modified, view.last_modified);
        copyField(&chunk.response.content_type, view.content_type);
    }
    session.next_offset += data_len;
    session.next_sequence += 1;
    switch (view.state) {
        types.State.complete, types.State.cancelled, types.State.failed => session.active = false,
        else => {},
    }
    return if (view.state == types.State.failed)
        @truncate(@as(u32, @bitCast(view.status)))
    else
        0;
}
