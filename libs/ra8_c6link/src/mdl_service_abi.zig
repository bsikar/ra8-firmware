//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI of the media-download service rules, the half of `ra8_c6link` that
//! runs on the ESP32-C6. `ra8_c6link_mdl_service.c` calls these on both
//! sides: the RA8 archive re-exports this file through `ra8_c6link_abi.zig`,
//! and `zig build c6-service` builds it alone into the archive the C6
//! `mdl_service` component links (#3195).

const Err = @import("abi_err.zig");
const mdl_pull = @import("internal/mdl_pull.zig");
const mdl_service_cancel = @import("internal/mdl_service_cancel.zig");
const mdl_service_next = @import("internal/mdl_service_next.zig");
const mdl_service_rules = @import("internal/mdl_service_rules.zig");
const mdl_service_start = @import("internal/mdl_service_start.zig");
const mdl_start_text = @import("internal/mdl_start_text.zig");
const mdl_types = @import("internal/mdl_types.zig");

/// `priv_c6link_mdl_service_field_valid`: one bounded single-line header.
pub export fn priv_c6link_mdl_service_field_valid(
    text: ?[*:0]const u8,
    cap: usize,
) callconv(.c) bool {
    if (cap == 0) return false;
    return mdl_service_rules.fieldValid(text, cap);
}

/// `priv_c6link_mdl_service_response_valid`: fixed terminal response metadata.
pub export fn priv_c6link_mdl_service_response_valid(
    response: ?*const mdl_service_rules.ResponseView,
) callconv(.c) bool {
    const metadata = response orelse return false;
    return mdl_service_rules.responseValid(metadata);
}

/// `priv_c6link_mdl_service_start_admit`: decode, check and copy one Start.
///
/// Refuses a malformed request, an invalid one, and any Start while a job is
/// active, in that order; `text` is written only once all three pass.
/// `out` is the empty request on every refusal.
pub export fn priv_c6link_mdl_service_start_admit(
    request: ?[*]const u8,
    request_len: usize,
    active: bool,
    text: ?*mdl_start_text.Text,
    out: ?*mdl_types.Request,
) callconv(.c) u16 {
    const backend_request = out orelse return Err.null_ptr;
    backend_request.* = .{};
    const in = request orelse return Err.null_ptr;
    const storage = text orelse return Err.null_ptr;
    backend_request.* = mdl_service_start.admit(in[0..request_len], active, storage) catch |e| return switch (e) {
        error.Malformed => Err.protocol_error,
        error.Invalid => Err.invalid_arg,
        error.Busy => Err.busy,
    };
    return Err.ok;
}

/// `priv_c6link_mdl_service_accepted`: encode the Accepted reply for a job.
/// `response_len` is zero and the buffer untouched on every refusal.
pub export fn priv_c6link_mdl_service_accepted(
    job_id: u32,
    format: u8,
    response: ?[*]u8,
    response_cap: usize,
    response_len: ?*usize,
) callconv(.c) u16 {
    const out_len = response_len orelse return Err.null_ptr;
    out_len.* = 0;
    const out = response orelse return Err.null_ptr;
    const bytes = mdl_service_start.accepted(job_id, format, out[0..response_cap]) catch return Err.invalid_size;
    out_len.* = bytes.len;
    return Err.ok;
}

/// `priv_c6link_mdl_decode_allocation_fits`: one aligned arena request.
pub export fn priv_c6link_mdl_decode_allocation_fits(
    used: usize,
    len: usize,
    capacity: usize,
) callconv(.c) bool {
    return mdl_service_rules.allocationFits(used, len, capacity);
}

/// `priv_c6link_mdl_decode_aligned_size`: that request's rounded size.
pub export fn priv_c6link_mdl_decode_aligned_size(len: usize) callconv(.c) usize {
    return mdl_service_rules.alignedSize(len);
}

/// `priv_c6link_mdl_service_response_size_ok`: a whole packed response fits.
pub export fn priv_c6link_mdl_service_response_size_ok(
    len: usize,
    response_cap: usize,
) callconv(.c) bool {
    return mdl_service_rules.responseSizeOk(len, response_cap);
}

/// `priv_c6link_mdl_service_next_admit`: decode and admit one NextRequest.
///
/// Refuses before the backend is asked for a byte: a malformed request, one
/// that does not name the active job at its offset, or one whose worst reply
/// would not fit `response_cap`. `max_bytes` is zero on every refusal.
pub export fn priv_c6link_mdl_service_next_admit(
    request: ?[*]const u8,
    request_len: usize,
    job: ?*const mdl_pull.JobView,
    response_cap: usize,
    max_bytes: ?*u32,
) callconv(.c) u16 {
    const granted = max_bytes orelse return Err.null_ptr;
    granted.* = 0;
    const in = request orelse return Err.null_ptr;
    const live = job orelse return Err.null_ptr;
    granted.* = mdl_service_next.admit(in[0..request_len], live, response_cap) catch |e| return switch (e) {
        error.Malformed => Err.protocol_error,
        error.Uncorrelated => Err.invalid_state,
        error.NoSpace => Err.invalid_size,
    };
    return Err.ok;
}

/// `priv_c6link_mdl_service_pack_chunk`: encode the Chunk for one pull.
/// `response_len` is zero on every refusal.
pub export fn priv_c6link_mdl_service_pack_chunk(
    reply: ?*const mdl_service_next.ReplyView,
    response: ?[*]u8,
    response_cap: usize,
    response_len: ?*usize,
) callconv(.c) u16 {
    const out_len = response_len orelse return Err.null_ptr;
    out_len.* = 0;
    const view = reply orelse return Err.null_ptr;
    const out = response orelse return Err.null_ptr;
    const bytes = mdl_service_next.pack(view, out[0..response_cap]) catch |e| return switch (e) {
        error.Missing => Err.null_ptr,
        error.NoSpace => Err.invalid_size,
    };
    out_len.* = bytes.len;
    return Err.ok;
}

/// `priv_c6link_mdl_service_cancel`: answer one CancelRequest for the job.
///
/// Decodes the request, checks it names the active job, and writes the
/// Cancelled acknowledgement. `response_len` is zero on every refusal and the
/// response buffer is untouched.
pub export fn priv_c6link_mdl_service_cancel(
    request: ?[*]const u8,
    request_len: usize,
    job: ?*const mdl_pull.JobView,
    response: ?[*]u8,
    response_cap: usize,
    response_len: ?*usize,
) callconv(.c) u16 {
    const out_len = response_len orelse return Err.null_ptr;
    out_len.* = 0;
    const in = request orelse return Err.null_ptr;
    const live = job orelse return Err.null_ptr;
    const out = response orelse return Err.null_ptr;
    const bytes = mdl_service_cancel.reply(in[0..request_len], live, out[0..response_cap]) catch |e| return switch (e) {
        error.Malformed => Err.protocol_error,
        error.Uncorrelated => Err.invalid_state,
        error.NoSpace => Err.invalid_size,
    };
    out_len.* = bytes.len;
    return Err.ok;
}

/// `priv_c6link_mdl_pull_end_offset`: offset past the body, 0 on overflow.
pub export fn priv_c6link_mdl_pull_end_offset(
    next_offset: u64,
    got: u16,
    overflowed: *bool,
) u64 {
    const end = mdl_pull.endOffset(next_offset, got);
    overflowed.* = end == null;
    return end orelse 0;
}

/// `priv_c6link_mdl_pull_coherent`: whether a backend pull may be packed.
pub export fn priv_c6link_mdl_pull_coherent(view: *const mdl_pull.PullView) bool {
    return mdl_pull.pullCoherent(view);
}

/// `priv_c6link_mdl_pull_advance`: job state after one packed pull.
pub export fn priv_c6link_mdl_pull_advance(
    next_offset: u64,
    next_sequence: u32,
    got: u16,
    complete: bool,
) mdl_pull.Advance {
    return mdl_pull.advance(next_offset, next_sequence, got, complete);
}
