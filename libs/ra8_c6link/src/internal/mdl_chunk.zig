//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The media-download chunk rules: what a decoded response has to look like
//! before any of it reaches caller memory, and the one copy that applies it.
//!
//! The protobuf codec stays in C, because the generated message layout is
//! protoc-c output and mirroring it by hand would be a silent-drift hazard.
//! What crosses into Zig is a `View`: the decoded field values, flat, with no
//! generated types in sight. Every rule about those values lives here.

const std = @import("std");

const request = @import("mdl_request.zig");
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

/// One decoded chunk response, as flat values rather than generated types.
///
/// A byte span is a pointer and a length because that is what the decoder
/// hands over; an absent span is a null pointer with a zero length. The four
/// header fields are the decoder's own NUL-terminated strings, which is why
/// they are not slices here.
pub const View = extern struct {
    job_id: u32 = 0,
    sequence: u32 = 0,
    offset: u64 = 0,
    total_bytes: u64 = 0,
    state: u8 = 0,
    status: i32 = 0,
    data: ?[*]const u8 = null,
    data_len: usize = 0,
    sha256: ?[*]const u8 = null,
    sha256_len: usize = 0,
    http_status: i32 = 0,
    retry_after: ?[*:0]const u8 = null,
    etag: ?[*:0]const u8 = null,
    last_modified: ?[*:0]const u8 = null,
    content_type: ?[*:0]const u8 = null,
};

fn present(text: ?[*:0]const u8) bool {
    const ptr = text orelse return false;
    return ptr[0] != 0;
}

/// Is every selected header absent, so a non-terminal chunk carries no metadata?
fn headersEmpty(view: *const View) bool {
    return view.retry_after != null and !present(view.retry_after) and
        view.etag != null and !present(view.etag) and
        view.last_modified != null and !present(view.last_modified) and
        view.content_type != null and !present(view.content_type);
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
        request.httpFieldValid(view.retry_after, Bound.retry_after) and
        request.httpFieldValid(view.etag, Bound.etag) and
        request.httpFieldValid(view.last_modified, Bound.http_date) and
        request.httpFieldValid(view.content_type, Bound.content_type);
}

/// Are the state-specific fields of one correlated chunk coherent?
///
/// Each state admits exactly one shape: DOWNLOADING carries data and no
/// digest, COMPLETE carries a digest and no data, CANCELLED carries neither,
/// FAILED carries a real status and neither. The totals are checked first so
/// a later bounded copy cannot be handed an end offset that overflowed.
pub fn semanticsValid(view: *const View) bool {
    if (view.data_len > std.math.maxInt(u64) - view.offset) return false;
    const end = view.offset + view.data_len;
    if (view.total_bytes != 0 and end > view.total_bytes) return false;
    if (!httpResponseValid(view)) return false;

    return switch (view.state) {
        types.State.downloading => view.status == 0 and
            view.data_len != 0 and view.sha256_len == 0,
        types.State.complete => view.status == 0 and
            view.data_len == 0 and
            view.sha256_len == types.Limit.sha256_bytes and
            view.sha256 != null and
            (view.total_bytes == 0 or view.total_bytes == end),
        types.State.cancelled => view.status == 0 and
            view.data_len == 0 and view.sha256_len == 0,
        types.State.failed => view.status > 0 and
            view.status <= std.math.maxInt(u16) and
            view.data_len == 0 and view.sha256_len == 0,
        else => false,
    };
}

/// Copy one header into fixed response storage, always terminated.
///
/// `httpResponseValid` already rejects anything longer than the storage, so a
/// source that reaches the cap here means validation was skipped. Truncate to
/// the last byte rather than run past the destination: the terminator is part
/// of the storage, so the usable text is one byte shorter than the array.
fn copyField(destination: []u8, source: ?[*:0]const u8) void {
    const ptr = source orelse return;
    const cap = destination.len - 1;
    var length: usize = 0;
    while (length < cap and ptr[length] != 0) : (length += 1) {}
    @memcpy(destination[0..length], ptr[0..length]);
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
    chunk.* = .{
        .job_id = view.job_id,
        .sequence = view.sequence,
        .offset = view.offset,
        .total_bytes = view.total_bytes,
        .state = view.state,
        .status = @truncate(@as(u32, @bitCast(view.status))),
        .data_len = @intCast(view.data_len),
        .has_sha256 = view.sha256_len == types.Limit.sha256_bytes,
    };
    if (view.data_len != 0) {
        const source = view.data.?;
        @memcpy(chunk.data[0..view.data_len], source[0..view.data_len]);
    }
    if (chunk.has_sha256) {
        const digest = view.sha256.?;
        @memcpy(&chunk.sha256, digest[0..types.Limit.sha256_bytes]);
    }
    if (view.state == types.State.complete) {
        chunk.response.status = view.http_status;
        copyField(&chunk.response.retry_after, view.retry_after);
        copyField(&chunk.response.etag, view.etag);
        copyField(&chunk.response.last_modified, view.last_modified);
        copyField(&chunk.response.content_type, view.content_type);
    }
    session.next_offset += view.data_len;
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
