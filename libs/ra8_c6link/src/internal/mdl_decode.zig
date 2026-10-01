//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Decoders for the media-download Accepted and Cancelled responses
//! (`proto/ra8_media_download.proto`), replacing the generated protobuf-c
//! unpack calls for them. They fill the same flat views the session rules
//! already judge, counting unknown fields the way protobuf-c did so the
//! zero-unknowns rule still holds.

const read = @import("mdl_wire_read.zig");
const session = @import("mdl_session.zig");

pub const Error = read.Error;

/// Field numbers of `ra8.mdl.Accepted`.
pub const Accepted = struct {
    pub const protocol_version: u32 = 1;
    pub const job_id: u32 = 2;
    pub const max_chunk_bytes: u32 = 3;
    pub const format: u32 = 4;
};

/// Field numbers of `ra8.mdl.Cancelled`.
pub const Cancelled = struct {
    pub const protocol_version: u32 = 1;
    pub const job_id: u32 = 2;
    pub const status: u32 = 3;
};

/// Decode an Accepted response. A repeated field keeps its last value.
pub fn accepted(bytes: []const u8) Error!session.AcceptedView {
    var view: session.AcceptedView = .{};
    var reader = read.Reader.init(bytes);
    while (try reader.next()) |field| switch (field.number) {
        Accepted.protocol_version => view.protocol_version = try field.uint32(),
        Accepted.job_id => view.job_id = try field.uint32(),
        Accepted.max_chunk_bytes => view.max_chunk_bytes = try field.uint32(),
        Accepted.format => view.format = try field.uint32(),
        else => view.unknown_fields +|= 1,
    };
    return view;
}

/// Decode a Cancelled response. A repeated field keeps its last value.
pub fn cancelled(bytes: []const u8) Error!session.CancelledView {
    var view: session.CancelledView = .{};
    var reader = read.Reader.init(bytes);
    while (try reader.next()) |field| switch (field.number) {
        Cancelled.protocol_version => view.protocol_version = try field.uint32(),
        Cancelled.job_id => view.job_id = try field.uint32(),
        Cancelled.status => view.status = try field.int32(),
        else => view.unknown_fields +|= 1,
    };
    return view;
}
