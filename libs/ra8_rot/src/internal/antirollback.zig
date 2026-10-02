//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The anti-rollback decisions, with no storage and no MMIO in them.
//!
//! Three questions, each answerable on its own: is this image a downgrade,
//! what does a raw counter word mean, and does an accepted version need
//! persisting. The durable counter lives behind the store vtable in
//! `antirollback_abi.zig`; everything here is decidable from two integers,
//! which is what lets `tests/antirollback_test.zig` drive it.
//!
//! The Thumb instruction width below belongs here for the same reason: the
//! fault handler's only real decision is how far to advance the stacked PC,
//! and that is a function of one halfword.

/// The durable counter's unprogrammed value. Extra-MRAM has no BlankCheck
/// command on this part, so a virgin word is recognised by reading it: a bench
/// run proved on EK-RA8D2 silicon that it reads back all-ones without
/// faulting.
pub const Nv = struct {
    pub const erased: u32 = 0xFFFF_FFFF;
};

/// First halfword encoding of Thumb instruction width (ARMv7-M ARM A5.1):
/// bits [15:11] at or above 0b11101 mean a 32-bit instruction.
pub const Thumb = struct {
    pub const halfword_mask: u16 = 0xF800;
    pub const wide_min: u16 = 0xE800;
};

/// The stored floor a raw counter word stands for. An erased word means the
/// device has accepted nothing yet, which is version 0: any image is `>= 0`,
/// so a fresh device launches its first authentic image.
pub fn storedFrom(raw: u32) u32 {
    return if (raw == Nv.erased) 0 else raw;
}

/// The pure downgrade policy. A strictly older image carries since-patched
/// defects and is denied; newer or equal is not a downgrade. Equal is
/// deliberately accepted so a same-version re-flash still boots.
pub fn accepts(image_version: u32, stored_min_version: u32) bool {
    return image_version >= stored_min_version;
}

/// Whether an accepted version has to be written down. The counter only ever
/// advances, so committing a version at or below the floor is a no-op and
/// costs no program cycle.
pub fn needsCommit(new_version: u32, stored: u32) bool {
    return new_version > stored;
}

/// How far to advance a stacked PC past the faulting load, in bytes, given
/// the first halfword of the instruction at that address.
pub fn instructionWidth(first_halfword: u16) u32 {
    return if ((first_halfword & Thumb.halfword_mask) >= Thumb.wide_min) 4 else 2;
}
