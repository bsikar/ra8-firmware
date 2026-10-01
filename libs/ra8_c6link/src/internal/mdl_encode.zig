//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Encoders for the media-download NextRequest and CancelRequest messages
//! (`proto/ra8_media_download.proto`), replacing the generated protobuf-c
//! pack calls for them. Field numbers come from the .proto; the output is
//! byte-identical to the reference encoder, which the vectors pin.

const wire = @import("mdl_wire.zig");

pub const Error = wire.Error;

/// `k_ra8_mdl_protocol_version` in `inc/ra8_mdl_protocol.h`.
pub const protocol_version: u32 = 3;

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
