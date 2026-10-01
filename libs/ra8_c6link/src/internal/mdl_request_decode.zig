//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Service-side decoder for the media-download Start, Next and Cancel requests
//! (`proto/ra8_media_download.proto`), replacing the generated protobuf-c
//! unpack. The service refuses a request carrying any field it does not
//! know, so an unknown field is refused here as malformed.

const encode = @import("mdl_encode.zig");
const pull = @import("mdl_pull.zig");
const read = @import("mdl_wire_read.zig");
const rules = @import("mdl_service_rules.zig");

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

/// Decode a StartRequest; its text is borrowed from `bytes`. A repeated
/// field keeps its last value. The format is read as the low 32 bits, as
/// protobuf-c reads an enum, so the rules refuse a negative one.
pub fn start(bytes: []const u8) Error!rules.StartView {
    var view: rules.StartView = .{};
    var reader = read.Reader.init(bytes);
    while (try reader.next()) |field| switch (field.number) {
        encode.Start.protocol_version => view.protocol_version = try field.uint32(),
        encode.Start.url => view.url = try field.bytes(),
        encode.Start.format => view.format = try field.uint32(),
        encode.Start.user_agent => view.user_agent = try field.bytes(),
        encode.Start.referer => view.referer = try field.bytes(),
        encode.Start.if_none_match => view.if_none_match = try field.bytes(),
        encode.Start.if_modified_since => view.if_modified_since = try field.bytes(),
        encode.Start.timeout_ms => view.timeout_ms = try field.uint32(),
        else => return error.Malformed,
    };
    return view;
}
