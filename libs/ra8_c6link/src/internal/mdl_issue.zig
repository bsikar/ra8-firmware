//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! What has to hold before a media-download request goes out.
//!
//! The session preconditions for `next` and `cancel`, and the one codec
//! self-consistency guard all three request encoders share. Nothing here
//! encodes anything: the generated protobuf codecs stay on the C side of the
//! membrane and hand this module plain lengths.

const types = @import("mdl_types.zig");

/// Bounds a request is held to before it is encoded.
pub const Bound = struct {
    /// Largest payload one chunk may carry, so the largest `next` may ask for.
    pub const chunk_data_max: u32 = types.Limit.chunk_data_max;
    /// Capacity of the link-owned request buffer: every bounded field plus
    /// 96 bytes of tag and varint headroom.
    pub const request_bytes_max: usize = 1568;
};

/// Why a request was refused before it was sent.
pub const Refusal = error{
    InvalidState,
    InvalidSize,
};

/// Does this session have a job the peer will still answer about?
///
/// An active session always carries a non-zero job id, so a zero one means
/// the caller kept a session across a failed start.
fn correlatable(session: *const types.Session) bool {
    return session.active and session.job_id != 0;
}

/// May `cancel` be issued for this session?
pub fn cancelAllowed(session: *const types.Session) Refusal!void {
    if (!correlatable(session)) return Refusal.InvalidState;
}

/// May `next` ask this session for `max_bytes`?
///
/// The ask is bounded twice over: by what the peer negotiated at accept time
/// and by the protocol's own chunk ceiling, because a peer is free to offer a
/// `max_chunk_bytes` larger than this build can receive.
pub fn nextAllowed(session: *const types.Session, max_bytes: u16) Refusal!void {
    try cancelAllowed(session);
    if (max_bytes == 0) return Refusal.InvalidSize;
    if (max_bytes > session.max_chunk_bytes) return Refusal.InvalidSize;
    if (max_bytes > Bound.chunk_data_max) return Refusal.InvalidSize;
}
