//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Correlation rules for the media-download RPC responses.
//!
//! The generated protoc-c message layouts stay in C; this module sees only
//! flattened field values. It decides whether an Accepted, Chunk or Cancelled
//! response belongs to the session that asked for it, and applies the one
//! state transition each accepted response earns.

const std = @import("std");
const types = @import("mdl_types.zig");

/// Protocol constants the correlation rules enforce.
pub const Bound = struct {
    /// Wire protocol revision every response must claim.
    pub const protocol_version: u32 = 3;
    /// Largest payload a single chunk may carry.
    pub const chunk_data_max: u32 = types.Limit.chunk_data_max;
};

/// Flattened `Ra8__Mdl__Accepted`.
pub const AcceptedView = extern struct {
    protocol_version: u32 = 0,
    job_id: u32 = 0,
    max_chunk_bytes: u32 = 0,
    format: u32 = 0,
    unknown_fields: u32 = 0,
};

/// Flattened correlation fields of `Ra8__Mdl__Chunk`.
pub const ChunkKeyView = extern struct {
    protocol_version: u32 = 0,
    job_id: u32 = 0,
    sequence: u32 = 0,
    offset: u64 = 0,
    data_len: u32 = 0,
    data_present: bool = false,
    unknown_fields: u32 = 0,
};

/// Flattened `Ra8__Mdl__Cancelled`.
pub const CancelledView = extern struct {
    protocol_version: u32 = 0,
    job_id: u32 = 0,
    status: i32 = 0,
    unknown_fields: u32 = 0,
};

/// Does an accepted response open a usable job for the format we asked for?
pub fn acceptedValid(view: *const AcceptedView, requested_format: u32) bool {
    return view.unknown_fields == 0 and
        view.protocol_version == Bound.protocol_version and
        view.job_id != 0 and
        view.max_chunk_bytes != 0 and
        view.max_chunk_bytes <= Bound.chunk_data_max and
        view.format == requested_format;
}

/// Open the session an accepted response just granted.
pub fn activate(view: *const AcceptedView, session: *types.Session, requested_format: u8) void {
    session.* = .{
        .job_id = view.job_id,
        .next_sequence = 0,
        .next_offset = 0,
        .max_chunk_bytes = view.max_chunk_bytes,
        .format = requested_format,
        .active = true,
    };
}

/// Does a chunk sit at exactly the place in the stream we are waiting on?
pub fn chunkCorrelates(
    view: *const ChunkKeyView,
    session: *const types.Session,
    requested_bytes: u32,
) bool {
    return view.unknown_fields == 0 and
        view.protocol_version == Bound.protocol_version and
        view.job_id == session.job_id and
        view.sequence == session.next_sequence and
        view.offset == session.next_offset and
        view.data_len <= requested_bytes and
        view.data_len <= Bound.chunk_data_max and
        (view.data_len == 0 or view.data_present);
}

/// Does a cancellation acknowledge the job we asked to cancel?
pub fn cancelledValid(view: *const CancelledView, session: *const types.Session) bool {
    return view.unknown_fields == 0 and
        view.protocol_version == Bound.protocol_version and
        view.job_id == session.job_id and
        view.status == 0;
}

/// Close a session a cancellation acknowledged.
pub fn deactivate(session: *types.Session) void {
    session.active = false;
}
