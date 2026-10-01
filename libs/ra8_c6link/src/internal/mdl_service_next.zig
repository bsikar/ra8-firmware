//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The service's side of one NextRequest, around the backend read the C
//! dispatcher still makes: `admit` decodes and checks the request and proves
//! every reply fits before any byte is pulled, and `pack` encodes the Chunk
//! for the pull once the pull rules have passed it.

const std = @import("std");
const bound = @import("mdl_chunk_bound.zig");
const pull = @import("mdl_pull.zig");
const reply_encode = @import("mdl_reply_encode.zig");
const request_decode = @import("mdl_request_decode.zig");
const rules = @import("mdl_service_rules.zig");
const types = @import("mdl_types.zig");

pub const AdmitError = error{ Malformed, Uncorrelated, NoSpace };
pub const PackError = error{ Missing, NoSpace };

/// One pull the C dispatcher has read and validated; `mdl_chunk_reply_t`.
pub const ReplyView = extern struct {
    offset: u64 = 0,
    total: u64 = 0,
    data: ?[*]const u8 = null,
    digest: ?[*]const u8 = null,
    response: ?*const rules.ResponseView = null,
    job_id: u32 = 0,
    sequence: u32 = 0,
    got: u16 = 0,
    complete: bool = false,
};

/// Decode `request`, check it names the active job at the acknowledged
/// offset, and check the worst reply fits `response_cap`. Returns the body
/// bound the peer granted.
pub fn admit(request: []const u8, job: *const pull.JobView, response_cap: usize) AdmitError!u32 {
    const view = try request_decode.next(request);
    if (!pull.nextCorrelates(&view, job)) return error.Uncorrelated;
    if (bound.worstCase(view.max_bytes) > response_cap) return error.NoSpace;
    return view.max_bytes;
}

/// Encode the Chunk for one admitted pull into `out`; returns its bytes.
///
/// `admit` already proved the reply fits, so NoSpace here means the caller
/// passed a smaller buffer than it admitted with.
pub fn pack(view: *const ReplyView, out: []u8) PackError![]const u8 {
    const body: []const u8 = if (view.got == 0) &.{} else (view.data orelse return error.Missing)[0..view.got];
    return reply_encode.chunk(out, .{
        .job_id = view.job_id,
        .sequence = view.sequence,
        .offset = view.offset,
        .data = body,
        .total = view.total,
        .terminal = if (view.complete) try terminal(view) else null,
    });
}

fn terminal(view: *const ReplyView) PackError!reply_encode.Terminal {
    const digest = view.digest orelse return error.Missing;
    const response = view.response orelse return error.Missing;
    return .{
        .sha256 = digest[0..types.Limit.sha256_bytes],
        .http_status = response.status,
        .retry_after = text(&response.retry_after),
        .etag = text(&response.etag),
        .last_modified = text(&response.last_modified),
        .content_type = text(&response.content_type),
    };
}

/// A terminated header as a slice, stopping at the buffer end if unterminated.
fn text(field: []const u8) []const u8 {
    return std.mem.sliceTo(field, 0);
}
