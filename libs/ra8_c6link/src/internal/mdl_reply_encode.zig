//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Service-side encoder for the media-download Cancelled response
//! (`proto/ra8_media_download.proto`), replacing the generated protobuf-c
//! pack. Byte-identical to the reference encoder, which the vectors pin.

const decode = @import("mdl_decode.zig");
const encode = @import("mdl_encode.zig");
const wire = @import("mdl_wire.zig");

pub const Error = wire.Error;

/// Encode a Cancelled acknowledgement with status zero, which proto3 leaves
/// off the wire. Returns the bytes written.
pub fn cancelled(buf: []u8, job_id: u32) Error![]const u8 {
    var w = wire.Writer.init(buf);
    try w.uint(decode.Cancelled.protocol_version, encode.protocol_version);
    try w.uint(decode.Cancelled.job_id, job_id);
    return w.written();
}
