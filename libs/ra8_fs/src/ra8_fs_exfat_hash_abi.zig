//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! exFAT name hashing, in its own unit so the consistency check can fold a
//! name without linking the whole write side: the hash is read-side work too
//! (the check verifies the stream entry's NameHash), and pulling the write
//! module into test binaries collides with the fake harness's stubs.

const std = @import("std");
const c = @import("fs_c.zig").c;
const upcase = @import("ra8_fs_upcase_abi.zig");

const csum_hi_bit: u16 = @intCast(c.k_exfat_csum_hi_bit);
const utf_byte_mask: u16 = @intCast(c.k_utf_byte_mask);
const utf_byte_shift: u5 = @intCast(c.k_utf_byte_shift);

/// One step of exFAT's rotate-right-then-add 16-bit checksum.
fn csumAdd(cs: u16, b: u8) u16 {
    const hi: u16 = if ((cs & 1) != 0) csum_hi_bit else 0;
    return hi +% (cs >> 1) +% @as(u16, b);
}

/// Fold the UP-CASED name, little-endian, one byte at a time: the spec's
/// definition exactly.
pub export fn priv_exfat_name_hash(name: [*c]const u16, nlen: u32) callconv(.c) u16 {
    var h: u16 = 0;
    var i: u32 = 0;
    while (i < nlen) : (i += 1) {
        const u: u16 = upcase.priv_exfat_upcase_unit(name[i]);
        h = csumAdd(h, @truncate(u & utf_byte_mask));
        h = csumAdd(h, @truncate(u >> utf_byte_shift));
    }
    return h;
}
