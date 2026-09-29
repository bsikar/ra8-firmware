//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for `ra8_path.h`: the untrusted-name policy exported to C.
//!
//! This file owns exactly what the pure core in `internal/path.zig` refuses
//! to know about: NUL-terminated pointers that may be null, caller capacities
//! measured in bytes, and the optional out-parameters. It makes no policy
//! decision of its own.

const std = @import("std");
const core = @import("internal/root.zig");
const policy = @import("internal/path.zig");

const Err = core.Err;

/// k_ra8_path_limits_t.
pub const Limits = struct {
    pub const segment_cap_min: u16 = policy.Policy.segment_cap_min;
    pub const path_cap: u16 = core.path_cap;
};

// ra8_path.h's two limits and the C `bool` the out-parameters point at,
// pinned here because this file is what the policy checks them against.
comptime {
    std.debug.assert(Limits.segment_cap_min == 2);
    std.debug.assert(Limits.path_cap == core.path_cap);
    std.debug.assert(@sizeOf(bool) == 1);
}

/// The caller's `char* out, size_t cap` pair as a slice, or null when the
/// pointer was null.
fn outSlice(out: ?[*]u8, cap: usize) ?[]u8 {
    const base = out orelse return null;
    return base[0..cap];
}

/// A `const char*` that may be null, as a slice without its NUL.
fn inSlice(value: ?[*:0]const u8) ?[]const u8 {
    const base = value orelse return null;
    return std.mem.span(base);
}

/// The one place the policy's refusals become `ra8_err.h` codes.
fn code(fault: policy.Fault) Err {
    return switch (fault) {
        error.CapTooSmall => core.err_invalid_size,
        error.NotOneSegment, error.EmptyParent => core.err_invalid_arg,
        error.WouldTruncate => core.err_no_mem,
    };
}

pub export fn ra8_path_sanitize_segment(
    raw: ?[*:0]const u8,
    out: ?[*]u8,
    cap: usize,
    out_verbatim: ?*u8,
) callconv(.c) Err {
    const buffer = outSlice(out, cap) orelse return core.err_null_ptr;

    const segment = policy.sanitizeSegment(inSlice(raw), buffer) catch |fault|
        return code(fault);
    buffer[segment.len] = 0;

    if (out_verbatim) |slot| slot.* = @intFromBool(segment.verbatim);
    return core.ok;
}

pub export fn ra8_path_join_under(
    parent: ?[*:0]const u8,
    seg: ?[*:0]const u8,
    out: ?[*]u8,
    cap: usize,
) callconv(.c) Err {
    const buffer = outSlice(out, cap) orelse return core.err_null_ptr;
    if (cap == 0) return core.err_invalid_size;
    buffer[0] = 0;

    const parent_value = inSlice(parent) orelse return core.err_null_ptr;
    const seg_value = inSlice(seg) orelse return core.err_null_ptr;

    const len = policy.joinUnder(parent_value, seg_value, buffer) catch |fault|
        return code(fault);
    buffer[len] = 0;
    return core.ok;
}

pub export fn ra8_path_contained(
    parent: ?[*:0]const u8,
    candidate: ?[*:0]const u8,
    out_contained: ?*u8,
) callconv(.c) Err {
    const parent_value = inSlice(parent) orelse return core.err_null_ptr;
    const candidate_value = inSlice(candidate) orelse return core.err_null_ptr;
    const slot = out_contained orelse return core.err_null_ptr;

    const verdict = policy.contained(parent_value, candidate_value) catch |fault|
        return code(fault);
    slot.* = @intFromBool(verdict);
    return core.ok;
}
