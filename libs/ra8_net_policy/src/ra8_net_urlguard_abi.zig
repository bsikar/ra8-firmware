//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for the URL and peer-address safety policy.
//!
//! This is the only file that speaks NUL-terminated strings and raw pointers.
//! It checks what a C caller may get wrong (a null pointer, a zero capacity),
//! turns the caller's storage into slices, and hands the decision to the rings
//! below, which work on slices throughout.

const std = @import("std");

const policy = @import("internal/policy.zig");
const root = @import("internal/root.zig");
const url_policy = @import("internal/url.zig");

const AddrClass = root.AddrClass;
const err = root.err;

/// Whether the URL carries a scheme the guard will fetch over.
export fn ra8_net_urlguard_scheme_allowed(url: ?[*:0]const u8) bool {
    const text = url orelse return false;
    return url_policy.schemeAllowed(std.mem.span(text));
}

/// Classify a peer address literal.
export fn ra8_net_urlguard_classify_ip(ip: ?[*:0]const u8) u8 {
    const text = ip orelse return @backingInt(AddrClass.unknown);
    return @backingInt(policy.classifyIp(std.mem.span(text)));
}

/// Whether an address of this class may be fetched.
///
/// A class value the guard does not publish is treated as it was in C: neither
/// public nor unknown, so it rides the caller's opt-in rather than being let
/// through on its own.
export fn ra8_net_urlguard_addr_fetchable(cls: u8, allow_private: bool) bool {
    const class = std.meta.intToEnum(AddrClass, cls) catch return allow_private;
    return policy.fetchable(class, allow_private);
}

/// Whether `add` more bytes would carry `have` past `cap`.
export fn ra8_net_urlguard_size_exceeds(have: u64, add: u64, cap: u64) bool {
    return policy.sizeExceeds(have, add, cap);
}

/// Copy the lower-cased authority of the URL into the caller's buffer.
export fn ra8_net_urlguard_host(url: ?[*:0]const u8, out: ?[*]u8, cap: usize) u16 {
    const buffer = out orelse return err.invalid_arg;
    if (cap == 0) return err.invalid_arg;
    buffer[0] = 0;
    const text = url orelse return err.invalid_arg;
    return url_policy.copyHost(std.mem.span(text), buffer[0..cap]);
}

/// Copy the path of the URL into the caller's buffer, query and fragment cut.
export fn ra8_net_urlguard_path(url: ?[*:0]const u8, out: ?[*]u8, cap: usize) u16 {
    const buffer = out orelse return err.invalid_arg;
    if (cap == 0) return err.invalid_arg;
    buffer[0] = 0;
    const text = url orelse return err.invalid_arg;
    return url_policy.copyPath(std.mem.span(text), buffer[0..cap]);
}
