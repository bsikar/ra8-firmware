//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Reading the Non-Secure image's self-describing root-of-trust header.
//!
//! The NS linker emits the header at a fixed offset past the NS vector
//! table, so the Secure verifier learns the signed body length without a
//! hand-encoded trailer address.

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
