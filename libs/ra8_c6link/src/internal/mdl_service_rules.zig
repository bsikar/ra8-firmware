//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Value rules the portable media-download service applies before it lets a
//! decoded request, a backend response, or a packed length reach anything with
//! a side effect. Every function here is pure: no arena, no backend, no codec.

const std = @import("std");

const types = @import("mdl_types.zig");

/// Bounds the service checks a request or response against.
pub const Bound = struct {
    pub const url_max: usize = 512;
    pub const user_agent_max: usize = 256;
    pub const referer_max: usize = 512;
    pub const etag_max: usize = 128;
    pub const http_date_max: usize = 64;
    pub const retry_after_max: usize = 64;
    pub const content_type_max: usize = 128;
    pub const timeout_ms_max: u32 = 60_000;
    pub const status_min: i32 = 100;
    pub const status_max: i32 = 599;
    /// Alignment every arena span is issued on.
    pub const decode_align: usize = 8;
    /// Highest format the service accepts on a Start request.
    pub const format_max: u32 = types.Format.rabook;
    /// Typed HTTP-artifact transfer protocol version this service speaks.
    pub const protocol_version: u32 = 3;
};

/// One decoded Start request, its text borrowed from the request bytes.
/// An absent header is empty, which proto3 leaves off the wire.
pub const StartView = struct {
    protocol_version: u32 = 0,
    url: []const u8 = "",
    format: u32 = 0,
    timeout_ms: u32 = 0,
    user_agent: []const u8 = "",
    referer: []const u8 = "",
    if_none_match: []const u8 = "",
    if_modified_since: []const u8 = "",
};

/// Terminal response metadata a backend fills in, as fixed storage.
pub const ResponseView = extern struct {
    status: i32 = 0,
    retry_after: [Bound.retry_after_max]u8 = @splat(0),
    etag: [Bound.etag_max]u8 = @splat(0),
    last_modified: [Bound.http_date_max]u8 = @splat(0),
    content_type: [Bound.content_type_max]u8 = @splat(0),
};

/// Length of `text` up to `cap`, which is `cap` when it never terminates.
///
/// Scans to the bound rather than taking a sentinel slice: a merely over-long
/// string has to be *refused*, and the sentinel form would panic on it.
fn span(text: [*:0]const u8, cap: usize) usize {
    var length: usize = 0;
    while (length < cap and text[length] != 0) : (length += 1) {}
    return length;
}

/// Whether `text` fits a buffer of `cap` with room for its terminator and
/// holds no byte that would cut it short or split it.
///
/// Refusing CR and LF is what stops a remote request from injecting extra
/// headers into the backend's own request. Refusing NUL keeps the terminated
/// copy the backend reads identical to what was checked.
pub fn textValid(text: []const u8, cap: usize) bool {
    if (text.len >= cap) return false;
    return std.mem.indexOfAny(u8, text, "\r\n\x00") == null;
}

/// Whether one terminated optional header is bounded single-line text.
///
/// Empty is valid and means the header is absent.
pub fn fieldValid(text: ?[*:0]const u8, cap: usize) bool {
    const ptr = text orelse return false;
    const length = span(ptr, cap);
    if (length >= cap) return false;
    return textValid(ptr[0..length], cap);
}

/// Whether fixed terminal response metadata is well formed.
///
/// Bounds every selected header independently, so a true result authorises
/// packing all of them.
pub fn responseValid(response: *const ResponseView) bool {
    if (response.status < Bound.status_min or response.status > Bound.status_max) return false;
    return fieldValid(@ptrCast(&response.retry_after), response.retry_after.len) and
        fieldValid(@ptrCast(&response.etag), response.etag.len) and
        fieldValid(@ptrCast(&response.last_modified), response.last_modified.len) and
        fieldValid(@ptrCast(&response.content_type), response.content_type.len);
}

/// Whether one decoded Start request may begin a job.
///
/// The URL has to be an https URL with something after the scheme: a bare
/// "https://" decodes fine and would reach the backend as a request for
/// nothing.
pub fn startValid(request: *const StartView) bool {
    const scheme = "https://";
    const url = request.url;
    if (url.len <= scheme.len or url.len >= Bound.url_max) return false;
    if (!std.mem.startsWith(u8, url, scheme)) return false;
    if (std.mem.indexOfScalar(u8, url, 0) != null) return false;

    return request.protocol_version == Bound.protocol_version and
        request.format <= Bound.format_max and
        request.timeout_ms <= Bound.timeout_ms_max and
        textValid(request.user_agent, Bound.user_agent_max) and
        textValid(request.referer, Bound.referer_max) and
        textValid(request.if_none_match, Bound.etag_max) and
        textValid(request.if_modified_since, Bound.http_date_max);
}

/// Whether one aligned arena request still fits.
///
/// Rounds the request the way the allocator does, and checks the rounded size
/// against the capacity before subtracting, so neither the rounding nor the
/// remaining-space arithmetic can wrap.
pub fn allocationFits(used: usize, len: usize, capacity: usize) bool {
    const mask = Bound.decode_align - 1;
    if (len > std.math.maxInt(usize) - mask) return false;
    const aligned = (len + mask) & ~mask;
    return aligned <= capacity and used <= capacity - aligned;
}

/// Size of one arena span, rounded to the published alignment.
///
/// Only meaningful once `allocationFits` has accepted the same length.
pub fn alignedSize(len: usize) usize {
    const mask = Bound.decode_align - 1;
    return (len + mask) & ~mask;
}

/// Whether a complete packed response fits the caller's buffer.
///
/// Zero is refused: the service packs a whole response or none, so a zero
/// length means the codec disagreed with itself rather than that there is
/// nothing to send.
pub fn responseSizeOk(len: usize, response_cap: usize) bool {
    return len != 0 and len <= response_cap;
}
