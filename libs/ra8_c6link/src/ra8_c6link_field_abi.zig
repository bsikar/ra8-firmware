//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the two field copies the RPC decoders use to lift a peer's
//! binary field into a fixed public struct: `priv_c6link_copy_str` and
//! `priv_c6link_copy_mac`. The sizing decisions are in `field_copy.zig`.

const std = @import("std");
const field_copy = @import("internal/field_copy.zig");
const header = @import("c6link_c.zig");

/// The public `ra8_c6link.h` view, re-exported for the host test.
pub const c = header.c;
/// protobuf-c's binary field.
pub const BinaryData = header.BinaryData;

comptime {
    if (@sizeOf(c.ra8_c6link_mac_t) != field_copy.Bound.mac_octets) @compileError("ra8_c6link_mac_t size drifted");
}

/// `priv_c6link_copy_str`: copy a text field into `cap` octets, terminated.
///
/// Always leaves `dst` terminated when it has any room, truncates a long
/// field, and returns the octets copied.
pub export fn priv_c6link_copy_str(dst: ?[*]u8, cap: u8, src: ?*const BinaryData) callconv(.c) u8 {
    const out = dst orelse return 0;
    if (cap == 0) return 0;
    out[0] = 0;
    const field = src orelse return 0;
    const data = field.data orelse return 0;

    const take = field_copy.strTake(field.len, cap);
    @memcpy(out[0..take], data[0..take]);
    out[take] = 0;
    return @intCast(take);
}

/// `priv_c6link_copy_mac`: copy a field that is exactly one hardware address.
///
/// Zeroes `dst` first, so a refused field never leaves a stale address.
pub export fn priv_c6link_copy_mac(dst: ?*c.ra8_c6link_mac_t, src: ?*const BinaryData) callconv(.c) bool {
    const out = dst orelse return false;
    out.* = std.mem.zeroes(c.ra8_c6link_mac_t);
    const field = src orelse return false;
    const data = field.data orelse return false;
    if (!field_copy.macAcceptable(field.len)) return false;

    @memcpy(&out.octet, data[0..field_copy.Bound.mac_octets]);
    return true;
}
