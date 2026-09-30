//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The pin-claim registry (#2825): one bit per port/pin, so two drivers cannot
//! configure the same physical pin without the second one being told.
//!
//! Pure logic. No MMIO, no logging, no C types. The membrane in
//! `src/pin_validator_abi.zig` puts the `ra8_err_t` contract back on and
//! does the logging the C did.

const std = @import("std");

/// Geometry of the packed `ra8_port_pin_t` id and the tables behind it.
pub const limits = struct {
    /// Ports 0..14 on the RA8 package.
    pub const port_count: u8 = 15;
    /// Pins per port.
    pub const pin_count: u8 = 16;
    /// Every port/pin pair that can be claimed.
    pub const slot_count: u16 = @as(u16, port_count) * @as(u16, pin_count);
    /// The port index lives in the high byte of the packed id.
    pub const port_shift: u4 = 8;
};

/// Why a packed pin id could not be turned into a slot.
pub const IndexError = error{
    /// The port half is past the last port on the package.
    InvalidPort,
    /// The pin half is past the last pin on a port.
    InvalidPin,
};

/// The slot a packed `ra8_port_pin_t` names, or why it names none.
pub fn slotOf(packed_pin: u16) IndexError!u16 {
    const port: u8 = @truncate(packed_pin >> limits.port_shift);
    const pin: u8 = @truncate(packed_pin);

    if (port >= limits.port_count) return IndexError.InvalidPort;
    if (pin >= limits.pin_count) return IndexError.InvalidPin;

    return (@as(u16, port) * @as(u16, limits.pin_count)) + @as(u16, pin);
}

/// One owner tag per slot, and one claimed bit per slot.
///
/// The owner tags are write-only by design: the C kept the same table and
/// nothing in the tree ever read it back. It is there to be legible in a
/// debugger when two drivers fight over a pin, so it is kept as the opaque
/// pointer the caller handed over and never dereferenced. That is also why
/// it is not a slice: forming one would mean reading a string this module
/// has no business reading.
pub const Registry = struct {
    claimed: std.StaticBitSet(limits.slot_count) = std.StaticBitSet(limits.slot_count).initEmpty(),
    owners: [limits.slot_count]?*const anyopaque = @splat(null),

    /// A slot that is already claimed cannot be claimed again.
    pub const ClaimError = error{AlreadyClaimed};

    pub fn claim(self: *Registry, slot: u16, owner: *const anyopaque) ClaimError!void {
        if (self.claimed.isSet(slot)) return ClaimError.AlreadyClaimed;
        self.claimed.set(slot);
        self.owners[slot] = owner;
    }

    /// Releasing a slot nobody holds is not an error, as it was not in the C.
    pub fn release(self: *Registry, slot: u16) void {
        self.claimed.unset(slot);
        self.owners[slot] = null;
    }

    pub fn isClaimed(self: *const Registry, slot: u16) bool {
        return self.claimed.isSet(slot);
    }

    pub fn reset(self: *Registry) void {
        self.claimed = std.StaticBitSet(limits.slot_count).initEmpty();
        self.owners = @splat(null);
    }
};
