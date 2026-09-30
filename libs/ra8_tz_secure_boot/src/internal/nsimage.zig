//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Reading the Non-Secure image's self-describing root-of-trust header.
//!
//! The NS linker emits the header at a fixed offset past the NS vector
//! table, so the Secure verifier learns the signed body length without a
//! hand-encoded trailer address.

const std = @import("std");
const regs = @import("regs.zig");

/// The 8-byte record the NS linker embeds.
pub const Header = extern struct {
    magic: u32,
    body_len: u32,
};

/// Read the signed body length out of an NS image.
///
/// Returns 0 as the deny sentinel when the header is missing or carries the
/// wrong magic, which is what every caller treats as "do not branch".
pub fn signedBodyLen(ns_image: [*]const u8) u32 {
    const at = ns_image + regs.NsRot.header_offset;
    const header: *align(1) const Header = @ptrCast(at);
    if (header.magic != regs.NsRot.magic) return 0;
    return header.body_len;
}

/// Lay a header into a buffer at the offset the NS linker uses.
fn place(buffer: []u8, magic: u32, body_len: u32) void {
    std.mem.writeInt(u32, buffer[regs.NsRot.header_offset..][0..4], magic, .little);
    std.mem.writeInt(u32, buffer[regs.NsRot.header_offset + 4 ..][0..4], body_len, .little);
}

test "a well-formed header reports its body length" {
    var image: [0x80]u8 = @splat(0);
    place(&image, regs.NsRot.magic, 0x4000);
    try std.testing.expectEqual(@as(u32, 0x4000), signedBodyLen(&image));
}

test "a wrong magic denies, whatever the length field says" {
    var image: [0x80]u8 = @splat(0);
    place(&image, 0xDEADBEEF, 0x4000);
    try std.testing.expectEqual(@as(u32, 0), signedBodyLen(&image));
}

test "an unwritten image denies" {
    const image: [0x80]u8 = @splat(0);
    try std.testing.expectEqual(@as(u32, 0), signedBodyLen(&image));
}

test "a zero body length is itself the deny sentinel" {
    var image: [0x80]u8 = @splat(0);
    place(&image, regs.NsRot.magic, 0);
    try std.testing.expectEqual(@as(u32, 0), signedBodyLen(&image));
}

test "the header sits just past the 16-slot vector table" {
    try std.testing.expectEqual(@as(usize, 16 * 4), regs.NsRot.header_offset);
    try std.testing.expectEqual(@as(usize, 8), @sizeOf(Header));
}
