//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Decoder for the media-download Chunk response
//! (`proto/ra8_media_download.proto`), replacing the generated protobuf-c
//! unpack. Every span borrows the response buffer, so nothing is allocated.
//! Absent fields read the way protobuf-c left them: a null body or digest,
//! and an empty (not null) header string.

const std = @import("std");

const read = @import("mdl_wire_read.zig");
const mdl_chunk = @import("mdl_chunk.zig");
const session = @import("mdl_session.zig");

pub const Error = read.Error;

/// Field numbers of `ra8.mdl.Chunk`.
pub const Field = struct {
    pub const protocol_version: u32 = 1;
    pub const job_id: u32 = 2;
    pub const sequence: u32 = 3;
    pub const offset: u32 = 4;
    pub const data: u32 = 5;
    pub const total_bytes: u32 = 6;
    pub const state: u32 = 7;
    pub const status: u32 = 8;
    pub const sha256: u32 = 9;
    pub const http_status: u32 = 10;
    pub const retry_after: u32 = 11;
    pub const etag: u32 = 12;
    pub const last_modified: u32 = 13;
    pub const content_type: u32 = 14;
};

/// One decoded chunk: what correlation reads, and what the rules read.
pub const Decoded = struct {
    key: session.ChunkKeyView,
    view: mdl_chunk.View,
};

const empty = mdl_chunk.Text.of("");

/// A bytes field as protobuf-c left it: null when empty.
fn span(bytes: []const u8) ?[*]const u8 {
    return if (bytes.len == 0) null else bytes.ptr;
}

/// The state enum. protobuf-c kept an int32 and the old view cut it to a
/// byte, so 258 read as DOWNLOADING; anything outside a byte is refused.
fn state(field: read.Field) Error!u8 {
    return std.math.cast(u8, try field.int32()) orelse error.Malformed;
}

/// Decode a Chunk response. A repeated field keeps its last value.
pub fn chunk(bytes: []const u8) Error!Decoded {
    var key: session.ChunkKeyView = .{};
    var view: mdl_chunk.View = .{
        .retry_after = empty,
        .etag = empty,
        .last_modified = empty,
        .content_type = empty,
    };
    var reader = read.Reader.init(bytes);
    while (try reader.next()) |field| switch (field.number) {
        Field.protocol_version => key.protocol_version = try field.uint32(),
        Field.job_id => view.job_id = try field.uint32(),
        Field.sequence => view.sequence = try field.uint32(),
        Field.offset => view.offset = try field.uint64(),
        Field.data => {
            const body = try field.bytes();
            view.data = span(body);
            view.data_len = body.len;
        },
        Field.total_bytes => view.total_bytes = try field.uint64(),
        Field.state => view.state = try state(field),
        Field.status => view.status = try field.int32(),
        Field.sha256 => {
            const digest = try field.bytes();
            view.sha256 = span(digest);
            view.sha256_len = digest.len;
        },
        Field.http_status => view.http_status = try field.int32(),
        Field.retry_after => view.retry_after = mdl_chunk.Text.of(try field.bytes()),
        Field.etag => view.etag = mdl_chunk.Text.of(try field.bytes()),
        Field.last_modified => view.last_modified = mdl_chunk.Text.of(try field.bytes()),
        Field.content_type => view.content_type = mdl_chunk.Text.of(try field.bytes()),
        else => key.unknown_fields +|= 1,
    };
    key.job_id = view.job_id;
    key.sequence = view.sequence;
    key.offset = view.offset;
    key.data_len = std.math.cast(u32, view.data_len) orelse return error.Malformed;
    key.data_present = view.data != null;
    return .{ .key = key, .view = view };
}
