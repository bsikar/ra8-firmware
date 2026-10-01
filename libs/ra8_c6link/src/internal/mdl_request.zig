//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The caller-facing half of the media download client: what a start request
//! must look like before anything is encoded, and where its optional headers
//! are staged for the wire.
//!
//! Nothing here touches the generated protobuf codecs. The rules are about the
//! caller's own argument contract, so they are stated once, in one place, and
//! the encoder downstream can assume every field it reads is already bounded.

const std = @import("std");

const types = @import("mdl_types.zig");

/// Protocol bounds for the optional HTTP fields, each including its NUL.
pub const Bound = struct {
    pub const url: usize = 512;
    pub const user_agent: usize = 256;
    pub const referer: usize = 512;
    pub const etag: usize = 128;
    pub const http_date: usize = 64;
    pub const timeout_ms_max: u32 = 60000;
    pub const https_prefix = "https://";
};

/// Why a request was refused.
pub const Refusal = error{
    NullPtr,
    InvalidArg,
};

/// A caller string as a slice, bounded by `cap` and never read past its NUL.
///
/// Null is absent. A string that fails to terminate inside its own bound is
/// over-long rather than unterminated caller memory, so it comes back as a
/// slice of exactly `cap` bytes and every caller treats that as a refusal.
fn span(text: ?[*:0]const u8, cap: usize) ?[]const u8 {
    const ptr = text orelse return null;
    var length: usize = 0;
    while (length < cap and ptr[length] != 0) : (length += 1) {}
    return ptr[0..length];
}

/// Is one optional field absent, or present, bounded and single-line?
///
/// An absent field is valid. A present one has to terminate inside its own
/// bound and carry no CR or LF, because the C6 service splices these straight
/// into a request head.
pub fn httpFieldValid(text: ?[*:0]const u8, cap: usize) bool {
    const ptr = text orelse return true;
    var index: usize = 0;
    while (index < cap) : (index += 1) {
        const byte = ptr[index];
        if (byte == 0) return true;
        if (byte == '\r' or byte == '\n') return false;
    }
    return false;
}

/// Does the URL name an HTTPS origin with something after the scheme?
fn urlValid(url: []const u8) bool {
    if (url.len == 0 or url.len >= Bound.url) return false;
    if (!std.mem.startsWith(u8, url, Bound.https_prefix)) return false;
    return url.len > Bound.https_prefix.len;
}

/// Check every caller-supplied field of a start request, and report the URL
/// length the encoder should copy, so it is bounded exactly once.
pub fn startRequestValid(request: ?*const types.Request) Refusal!usize {
    const req = request orelse return Refusal.NullPtr;
    const url = span(req.url, Bound.url) orelse return Refusal.NullPtr;
    if (!urlValid(url)) return Refusal.InvalidArg;

    if (req.format > types.Format.rabook) return Refusal.InvalidArg;
    if (req.http.timeout_ms > Bound.timeout_ms_max) return Refusal.InvalidArg;
    if (!httpFieldValid(req.http.user_agent, Bound.user_agent)) return Refusal.InvalidArg;
    if (!httpFieldValid(req.http.referer, Bound.referer)) return Refusal.InvalidArg;
    if (!httpFieldValid(req.http.if_none_match, Bound.etag)) return Refusal.InvalidArg;
    if (!httpFieldValid(req.http.if_modified_since, Bound.http_date)) return Refusal.InvalidArg;
    return url.len;
}

/// Fixed storage for the four optional request headers.
///
/// An absent header travels as an empty string rather than as a pointer into
/// caller memory, so the encoder never has to ask whether a field is there.
pub const Headers = extern struct {
    user_agent: [Bound.user_agent]u8,
    referer: [Bound.referer]u8,
    etag: [Bound.etag]u8,
    http_date: [Bound.http_date]u8,
};

fn stage(destination: []u8, text: ?[*:0]const u8) void {
    const source = span(text, destination.len) orelse return;
    @memcpy(destination[0..source.len], source);
    destination[source.len] = 0;
}

/// Copy every present header into caller-owned fixed storage.
///
/// Every field was bounded by `startRequestValid` first, so each copy fits and
/// this cannot fail.
pub fn stageHeaders(http: *const types.HttpPolicy, out: *Headers) void {
    out.* = std.mem.zeroes(Headers);
    stage(&out.user_agent, http.user_agent);
    stage(&out.referer, http.referer);
    stage(&out.etag, http.if_none_match);
    stage(&out.http_date, http.if_modified_since);
}
