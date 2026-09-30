//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Lexical URL policy: the scheme allowlist, and the authority and path the
//! caller is handed back. Purely lexical over caller-owned storage, so no
//! allocation, no network call, and no name resolution happen here.

const ascii = @import("ascii.zig");
const root = @import("root.zig");

const err = root.err;

/// Schemes the guard will fetch over, and the separator anchoring every parse.
const scheme = struct {
    pub const http = "http://";
    pub const https = "https://";
    pub const authority_sep = "://";
};

/// Whether `url` carries a scheme the guard will fetch over.
pub fn schemeAllowed(url: []const u8) bool {
    if (url.len == 0) return false;
    return ascii.startsWithCi(url, scheme.http) or ascii.startsWithCi(url, scheme.https);
}

/// The authority of `url`: everything after the first `://` and after any
/// userinfo that sits ahead of the first path separator.
pub fn authority(url: []const u8) ?[]const u8 {
    const sep = indexOfSep(url) orelse return null;
    const host = url[sep + scheme.authority_sep.len ..];
    const at = indexOfScalar(host, '@') orelse return host;
    const path = indexOfAny(host, "/?#");
    if (path != null and at > path.?) return host;
    return host[at + 1 ..];
}

/// Copy the lower-cased authority of `url` into `out`, NUL-terminated.
///
/// The port is kept: a politeness policy and a same-origin redirect check are
/// both scoped per (scheme, host, port), so a different port is a different
/// origin. Only userinfo is dropped, in `authority`.
pub fn copyHost(url: []const u8, out: []u8) u16 {
    out[0] = 0;
    const host = authority(url) orelse return err.not_found;
    var n: usize = 0;
    while (n < host.len and host[n] != '/' and host[n] != '?' and host[n] != '#') {
        if ((n + 1) >= out.len) {
            out[0] = 0;
            return err.no_mem;
        }
        out[n] = ascii.lower(host[n]);
        n += 1;
    }
    out[n] = 0;
    if (n == 0) return err.not_found;
    return err.ok;
}

/// Copy the path of `url` into `out`, NUL-terminated, query and fragment cut.
///
/// A URL with no path at all yields `/`, so a caller never has to special-case
/// the empty path when it builds a request line.
pub fn copyPath(url: []const u8, out: []u8) u16 {
    out[0] = 0;
    const sep = indexOfSep(url) orelse return err.not_found;
    const host = url[sep + scheme.authority_sep.len ..];
    const start = indexOfScalar(host, '/') orelse return emptyPath(out);
    const path = host[start..];
    var n: usize = 0;
    while (n < path.len and path[n] != '?' and path[n] != '#') {
        if ((n + 1) >= out.len) {
            out[0] = 0;
            return err.no_mem;
        }
        out[n] = path[n];
        n += 1;
    }
    out[n] = 0;
    return err.ok;
}

fn emptyPath(out: []u8) u16 {
    if (out.len < 2) return err.no_mem;
    out[0] = '/';
    out[1] = 0;
    return err.ok;
}

fn indexOfSep(url: []const u8) ?usize {
    const sep = scheme.authority_sep;
    if (url.len < sep.len) return null;
    for (0..(url.len - sep.len) + 1) |i| {
        if (std_eql(url[i..][0..sep.len], sep)) return i;
    }
    return null;
}

fn std_eql(a: []const u8, b: []const u8) bool {
    if (a.len != b.len) return false;
    for (a, b) |x, y| {
        if (x != y) return false;
    }
    return true;
}

fn indexOfScalar(s: []const u8, needle: u8) ?usize {
    for (s, 0..) |c, i| {
        if (c == needle) return i;
    }
    return null;
}

fn indexOfAny(s: []const u8, set: []const u8) ?usize {
    for (s, 0..) |c, i| {
        for (set) |want| {
            if (c == want) return i;
        }
    }
    return null;
}
