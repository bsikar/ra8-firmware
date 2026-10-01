//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Service-side pull rules for the media-download protocol.
//!
//! Everything here answers one question about a Next or Cancel dispatch
//! without touching the wire: whether the request correlates with the job the
//! service is running, whether what the backend handed back is coherent, and
//! where the job stands afterwards. Decoding the generated message and packing
//! the reply stay in C at the membrane; these are the rules that decide
//! whether either is worth doing.

const std = @import("std");

/// Protocol constants this layer enforces.
pub const Bound = struct {
    pub const protocol_version: u32 = 3;
    pub const chunk_data_max: u32 = 1024;
    pub const sequence_max: u32 = std.math.maxInt(u32);
};

/// The correlation state of the one job a service may be running.
pub const JobView = extern struct {
    next_offset: u64 = 0,
    active_job_id: u32 = 0,
    active: bool = false,
};

/// A decoded NextRequest, flattened.
pub const NextRequestView = extern struct {
    acknowledged_offset: u64 = 0,
    protocol_version: u32 = 0,
    job_id: u32 = 0,
    max_bytes: u32 = 0,
};

/// A decoded CancelRequest, flattened.
pub const CancelRequestView = extern struct {
    protocol_version: u32 = 0,
    job_id: u32 = 0,
};

/// One backend pull plus the service state it has to agree with.
pub const PullView = extern struct {
    next_offset: u64 = 0,
    total: u64 = 0,
    next_sequence: u32 = 0,
    max_data: u32 = 0,
    got: u16 = 0,
    complete: bool = false,
    response_valid: bool = false,
    response_status: i32 = 0,
};

/// Where the job stands after a pull the caller has already packed.
pub const Advance = extern struct {
    next_offset: u64 = 0,
    next_sequence: u32 = 0,
    active: bool = false,
};

/// Offset just past the returned body, or null when it would wrap.
///
/// A backend that overflows the offset space is not merely at the end: it has
/// lost track of where it is, so the pull is refused rather than clamped.
pub fn endOffset(next_offset: u64, got: u16) ?u64 {
    return std.math.add(u64, next_offset, got) catch null;
}

/// Whether the declared total agrees with where this pull ends.
///
/// A terminal pull has to close the artifact exactly. A non-terminal one may
/// leave `total` unset (zero, meaning unknown), but when it is declared the
/// pull cannot already have run past it.
pub fn totalCoherent(end_offset: u64, total: u64, complete: bool) bool {
    if (complete) return end_offset == total;
    if (total == 0) return true;
    return end_offset <= total;
}

/// Whether a NextRequest may act on this service's active job.
pub fn nextCorrelates(request: *const NextRequestView, job: *const JobView) bool {
    return request.protocol_version == Bound.protocol_version and
        job.active and
        request.job_id == job.active_job_id and
        request.acknowledged_offset == job.next_offset and
        request.max_bytes != 0 and
        request.max_bytes <= Bound.chunk_data_max;
}

/// Whether a CancelRequest may act on this service's active job.
pub fn cancelCorrelates(request: *const CancelRequestView, job: *const JobView) bool {
    return request.protocol_version == Bound.protocol_version and
        job.active and
        request.job_id == job.active_job_id;
}

/// Whether what the backend returned can be packed as the next Chunk.
///
/// The two shapes are exclusive: a data pull carries bytes and no terminal
/// metadata, a terminal pull carries metadata and no bytes. `next_sequence`
/// saturating is refused here because the packed Chunk would otherwise reuse
/// a sequence number the peer has already seen.
pub fn pullCoherent(view: *const PullView) bool {
    const end = endOffset(view.next_offset, view.got) orelse return false;
    if (view.got > view.max_data) return false;
    if (view.complete != (view.got == 0)) return false;
    if (view.next_sequence == Bound.sequence_max) return false;
    if (!totalCoherent(end, view.total, view.complete)) return false;
    if (view.complete) return view.response_valid;
    return view.response_status == 0;
}

/// Job state after a pull was read, packed, and sent.
pub fn advance(next_offset: u64, next_sequence: u32, got: u16, complete: bool) Advance {
    return .{
        .next_offset = next_offset + got,
        .next_sequence = next_sequence + 1,
        .active = !complete,
    };
}
