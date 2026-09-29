//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Untrusted-name policy: the pure string decisions behind `ra8_path.h`.
//!
//! Everything here works on slices of caller storage. No filesystem call, no
//! allocation, no symlink resolution, no global state, so the host tests can
//! drive every branch directly. The ABI file owns the NUL-terminated C
//! boundary; this file owns what a safe name is.

const std = @import("std");

/// What the policy can refuse. The ABI file maps each one onto its
/// `ra8_err.h` code; nothing in here knows those numbers.
pub const Fault = error{
    /// `cap` was below `Policy.segment_cap_min`.
    CapTooSmall,
    /// The segment was empty, `.`, `..`, or carried a `/`.
    NotOneSegment,
    /// The joined path would not fit, and a shortened path is a different
    /// path.
    WouldTruncate,
    /// The parent was empty or only slashes, so it contains nothing.
    EmptyParent,
};

/// Hard bounds and the alphabet the policy enforces.
pub const Policy = struct {
    /// k_ra8_path_segment_cap_min: one character plus its NUL.
    pub const segment_cap_min: usize = 2;
    /// Name substituted when a candidate sanitises to nothing usable.
    pub const fallback: []const u8 = "item";
    /// Byte every disallowed input byte becomes.
    pub const replacement: u8 = '_';
    /// Directory separator a single segment may never contain.
    pub const separator: u8 = '/';
    /// Reserved device base names matched in full, case-folded.
    pub const reserved_exact = [_][]const u8{ "con", "prn", "aux", "nul" };
    /// Reserved device prefixes that take a 1-9 suffix, case-folded.
    pub const reserved_numbered = [_][]const u8{ "com", "lpt" };
    /// Longest reserved base the classifier keeps, including its NUL in C.
    pub const reserved_base_max: usize = 8;
    /// Length of a COMx / LPTx name.
    pub const reserved_len: usize = 4;
    /// Index of the digit in COMx / LPTx.
    pub const reserved_digit_at: usize = 3;
};

/// What `sanitizeSegment` produced: the bytes written, and whether the
/// candidate survived untouched and untruncated.
pub const Segment = struct {
    len: usize,
    verbatim: bool,
};

fn lowerAscii(c: u8) u8 {
    return if (c >= 'A' and c <= 'Z') c + ('a' - 'A') else c;
}

/// An ASCII letter, digit, dot, dash or underscore may stand verbatim.
pub fn allowedChar(c: u8) bool {
    return (c >= 'A' and c <= 'Z') or (c >= 'a' and c <= 'z') or
        (c >= '0' and c <= '9') or c == '.' or c == '-' or c == '_';
}

/// True when a name carries no usable segment: empty, `.` or `..`.
pub fn dotSegment(name: []const u8) bool {
    return name.len == 0 or std.mem.eql(u8, name, ".") or std.mem.eql(u8, name, "..");
}

/// The case-folded base name, up to the first `.`, bounded the way the C
/// classifier's fixed buffer bounded it.
pub fn baseOf(name: []const u8, out: []u8) []u8 {
    const room = @min(out.len, Policy.reserved_base_max) - 1;
    var i: usize = 0;
    while (i < name.len and name[i] != '.' and i < room) : (i += 1) {
        out[i] = lowerAscii(name[i]);
    }
    return out[0..i];
}

/// True when a name's base is `con`, `prn`, `aux`, `nul`, `comN` or `lptN`.
pub fn reservedBase(name: []const u8) bool {
    var storage: [Policy.reserved_base_max]u8 = undefined;
    const base = baseOf(name, &storage);
    for (Policy.reserved_exact) |reserved| {
        if (std.mem.eql(u8, base, reserved)) return true;
    }
    if (base.len != Policy.reserved_len) return false;
    const digit = base[Policy.reserved_digit_at];
    if (digit < '1' or digit > '9') return false;
    for (Policy.reserved_numbered) |prefix| {
        if (std.mem.startsWith(u8, base, prefix)) return true;
    }
    return false;
}

/// Copy a candidate into `out`, replacing every disallowed byte. Returns the
/// bytes written and whether nothing was replaced or dropped. A null
/// candidate writes nothing and is never verbatim.
fn copySanitised(raw: ?[]const u8, out: []u8) Segment {
    const candidate = raw orelse return .{ .len = 0, .verbatim = false };
    const room = out.len - 1;
    const kept = @min(candidate.len, room);
    var clean = kept == candidate.len;
    for (candidate[0..kept], out[0..kept]) |in, *slot| {
        if (allowedChar(in)) {
            slot.* = in;
        } else {
            slot.* = Policy.replacement;
            clean = false;
        }
    }
    return .{ .len = kept, .verbatim = clean };
}

/// Prepend `_` in place, dropping the tail that no longer fits.
fn prependUnderscore(out: []u8, len: usize) usize {
    const keep = @min(len, out.len - 2);
    std.mem.copyBackwards(u8, out[1 .. keep + 1], out[0..keep]);
    out[0] = Policy.replacement;
    return keep + 1;
}

/// ra8_path_sanitize_segment: rewrite an untrusted candidate into one
/// filesystem-safe segment. `out` is the caller's buffer, NUL excluded from
/// the returned length; the ABI file terminates it.
pub fn sanitizeSegment(raw: ?[]const u8, out: []u8) Fault!Segment {
    if (out.len < Policy.segment_cap_min) return error.CapTooSmall;

    var result = copySanitised(raw, out);
    if (dotSegment(out[0..result.len])) {
        const kept = @min(Policy.fallback.len, out.len - 1);
        @memcpy(out[0..kept], Policy.fallback[0..kept]);
        return .{ .len = kept, .verbatim = false };
    }
    if (reservedBase(out[0..result.len])) {
        result.len = prependUnderscore(out, result.len);
        result.verbatim = false;
    }
    return result;
}

/// True when a segment would span more than one directory level.
pub fn hasSeparator(seg: []const u8) bool {
    return std.mem.indexOfScalar(u8, seg, Policy.separator) != null;
}

/// ra8_path_join_under: compose `parent` + `/` + `seg` into `out`, refusing
/// anything that is not a single safe segment and refusing to truncate.
/// Returns the bytes written, NUL excluded.
pub fn joinUnder(parent: []const u8, seg: []const u8, out: []u8) Fault!usize {
    if (dotSegment(seg) or hasSeparator(seg)) return error.NotOneSegment;

    const need = parent.len + 1 + seg.len + 1;
    if (need > out.len) return error.WouldTruncate;

    @memcpy(out[0..parent.len], parent);
    out[parent.len] = Policy.separator;
    @memcpy(out[parent.len + 1 ..][0..seg.len], seg);
    return parent.len + 1 + seg.len;
}

/// `parent` with its trailing slashes removed.
pub fn trimmedParent(parent: []const u8) []const u8 {
    var len = parent.len;
    while (len > 0 and parent[len - 1] == Policy.separator) : (len -= 1) {}
    return parent[0..len];
}

/// ra8_path_contained: lexical containment with the directory boundary
/// significant, so `/a/b` contains `/a/b` and `/a/b/c` but not `/a/bb`.
pub fn contained(parent: []const u8, candidate: []const u8) Fault!bool {
    const base = trimmedParent(parent);
    if (base.len == 0) return error.EmptyParent;
    if (!std.mem.startsWith(u8, candidate, base)) return false;
    if (candidate.len == base.len) return true;
    return candidate[base.len] == Policy.separator;
}
