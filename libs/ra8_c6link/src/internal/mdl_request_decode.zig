//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Service-side decoder for the media-download NextRequest and CancelRequest
//! (`proto/ra8_media_download.proto`), replacing the generated protobuf-c
//! unpack. The service refuses a request carrying any field it does not
//! know, so an unknown field is refused here as malformed.

const encode = @import("mdl_encode.zig");
const pull = @import("mdl_pull.zig");
const read = @import("mdl_wire_read.zig");

pub const Error = read.Error;

/// Decode a CancelRequest. A repeated field keeps its last value.
pub fn cancel(bytes: []const u8) Error!pull.CancelRequestView {
    var view: pull.CancelRequestView = .{};
    var reader = read.Reader.init(bytes);
    while (try reader.next()) |field| switch (field.number) {
        encode.Cancel.protocol_version => view.protocol_version = try field.uint32(),
        encode.Cancel.job_id => view.job_id = try field.uint32(),
        else => return error.Malformed,
    };
    return view;
}

/// Decode a NextRequest. A repeated field keeps its last value.
pub fn next(bytes: []const u8) Error!pull.NextRequestView {
    var view: pull.NextRequestView = .{};
    var reader = read.Reader.init(bytes);
    while (try reader.next()) |field| switch (field.number) {
        encode.Next.protocol_version => view.protocol_version = try field.uint32(),
        encode.Next.job_id => view.job_id = try field.uint32(),
        encode.Next.acknowledged_offset => view.acknowledged_offset = try field.uint64(),
        encode.Next.max_bytes => view.max_bytes = try field.uint32(),
        else => return error.Malformed,
    };
    return view;
}
