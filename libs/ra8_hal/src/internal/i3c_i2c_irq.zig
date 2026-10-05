//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! I3C legacy-I2C controller interrupt plumbing (RA8FW-693): the BIE and
//! NTIE groups toggled when a handler is attached or detached, and the
//! error-dispatch decision.

/// HUM 40.2 BIE NACKDIE | TENDIE | ALIE | TODIE, p 2484.
pub const bie_attached: u32 = (1 << 4) | (1 << 8) | (1 << 16) | (1 << 20);
/// HUM 40.2 NTIE TDBEIE0 | RDBFIE0, p 2488.
pub const ntie_attached: u32 = (1 << 0) | (1 << 1);

/// Enable the polling driver's interrupt group, or clear it on detach.
pub fn setEnables(bie: *volatile u32, ntie: *volatile u32, attached: bool) void {
    bie.* = if (attached) bie_attached else 0;
    ntie.* = if (attached) ntie_attached else 0;
}

/// Fire the callback only for a non-empty error mask with a handler set.
pub fn shouldDispatch(mask: u8, has_cb: bool) bool {
    return mask != 0 and has_cb;
}
