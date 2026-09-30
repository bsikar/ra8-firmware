//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The opaque slot handle the secure world vends to Non-Secure callers.
//!
//! A handle stands in for a vault slot index so the index itself never crosses
//! the veneer. It is salted per boot, so two boots hand out different handles
//! for the same slot, and a caller who collects many handles cannot difference
//! them back into the salt: the salt is rotated before the slot is mixed in,
//! which decorrelates its low half from its high half.
//!
//! This is obfuscation, not authentication. What actually keeps a forged handle
//! out is `key_import.resolve`, which only ever answers with a slot that is
//! live in the allocator bitmap.

const std = @import("std");

/// Salt seeds and mixing constants.
pub const Mix = struct {
    /// Boot salt seed.
    pub const initial_salt: u32 = 0xA5A5A5A5;
    /// Forces bit 31 high so a handle never collides with the zero sentinel.
    pub const high_bit: u32 = 0x8000_0000;
    /// Mixed into the salt on every reroll.
    pub const reroll_xor: u32 = 0xDEAD_BEEF;
    /// Salt rotation before the slot index is mixed in.
    pub const rotate_bits: u5 = 13;
    /// Salt rotation on reroll.
    pub const reroll_rotate: u5 = 7;
};

/// The reserved invalid-handle value. No live slot ever resolves to it.
pub const zero: u32 = 0;

var salt: u32 = Mix.initial_salt;

/// The handle for `slot` under the current salt.
///
/// Deterministic for a fixed (slot, salt) pair, and always non-zero.
pub fn forSlot(slot: u16) u32 {
    const mixed = std.math.rotl(u32, salt, Mix.rotate_bits);
    return (@as(u32, slot) ^ mixed) | Mix.high_bit;
}

/// Reroll the salt, so handles issued before this call stop resolving.
///
/// A rerolled salt of zero falls back to the seed: zero would make every
/// handle the bare slot index with bit 31 set, which is exactly the
/// correlation the salt exists to prevent.
pub fn reroll() void {
    salt = std.math.rotl(u32, salt, Mix.reroll_rotate) ^ Mix.reroll_xor;
    if (salt == 0) salt = Mix.initial_salt;
}

/// The live salt. For tests and for a caller proving two boots differ.
pub fn current() u32 {
    return salt;
}
