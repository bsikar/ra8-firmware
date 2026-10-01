//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Encoders for the media-download StartRequest, NextRequest and CancelRequest
//! messages
//! (`proto/ra8_media_download.proto`), replacing the generated protobuf-c
//! pack calls for them. Field numbers come from the .proto; the output is
//! byte-identical to the reference encoder, which the vectors pin.

const wire = @import("mdl_wire.zig");

pub const Error = wire.Error;

/// `k_ra8_mdl_protocol_version` in `inc/ra8_mdl_protocol.h`.
pub const protocol_version: u32 = 3;

/// Field numbers of `ra8.mdl.StartRequest`.
pub const Start = struct {
    pub const protocol_version: u32 = 1;
    pub const url: u32 = 2;
    pub const format: u32 = 3;
    pub const user_agent: u32 = 4;
    pub const referer: u32 = 5;
    pub const if_none_match: u32 = 6;
    pub const if_modified_since: u32 = 7;
    pub const timeout_ms: u32 = 8;
};

/// A StartRequest's content as slices. An absent header is empty, which
/// proto3 leaves off the wire exactly as protobuf-c did with "".
pub const StartFields = struct {
    url: []const u8,
    format: u8 = 0,
    user_agent: []const u8 = "",
    referer: []const u8 = "",
    if_none_match: []const u8 = "",
    if_modified_since: []const u8 = "",
    timeout_ms: u32 = 0,
};

/// Field numbers of `ra8.mdl.NextRequest`.
pub const Next = struct {
    pub const protocol_version: u32 = 1;
    pub const job_id: u32 = 2;
    pub const acknowledged_offset: u32 = 3;
    pub const max_bytes: u32 = 4;
};

/// Field numbers of `ra8.mdl.CancelRequest`.
pub const Cancel = struct {
    pub const protocol_version: u32 = 1;
    pub const job_id: u32 = 2;
};

/// Encode a StartRequest into `buf`; returns the bytes written.
pub fn start(buf: []u8, fields: StartFields) Error![]const u8 {
    var w = wire.Writer.init(buf);
    try w.uint(Start.protocol_version, protocol_version);
    try w.bytes(Start.url, fields.url);
    try w.uint(Start.format, fields.format);
    try w.bytes(Start.user_agent, fields.user_agent);
    try w.bytes(Start.referer, fields.referer);
    try w.bytes(Start.if_none_match, fields.if_none_match);
    try w.bytes(Start.if_modified_since, fields.if_modified_since);
    try w.uint(Start.timeout_ms, fields.timeout_ms);
    return w.written();
}

/// Encode a NextRequest into `buf`; returns the bytes written.
pub fn next(buf: []u8, job_id: u32, acknowledged_offset: u64, max_bytes: u32) Error![]const u8 {
    var w = wire.Writer.init(buf);
    try w.uint(Next.protocol_version, protocol_version);
    try w.uint(Next.job_id, job_id);
    try w.uint(Next.acknowledged_offset, acknowledged_offset);
    try w.uint(Next.max_bytes, max_bytes);
    return w.written();
}

/// Encode a CancelRequest into `buf`; returns the bytes written.
pub fn cancel(buf: []u8, job_id: u32) Error![]const u8 {
    var w = wire.Writer.init(buf);
    try w.uint(Cancel.protocol_version, protocol_version);
    try w.uint(Cancel.job_id, job_id);
    return w.written();
}
