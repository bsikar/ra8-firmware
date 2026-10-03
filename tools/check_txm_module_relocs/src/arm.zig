//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The ARM relocation types, and which of them survive a module being
//! loaded somewhere other than where it was linked.
//!
//! The numbers are the ELF for the Arm Architecture ABI's.

/// A relocation type as it sits in the low byte of `r_info`.
pub const Kind = u8;

pub const none: Kind = 0;
pub const abs32: Kind = 2;
pub const rel32: Kind = 3;
pub const abs16: Kind = 5;
pub const abs12: Kind = 6;
pub const abs8: Kind = 8;
pub const sbrel32: Kind = 9;
pub const gotoff32: Kind = 24;
pub const base_prel: Kind = 25;
pub const got_brel: Kind = 26;
pub const target1: Kind = 38;
pub const prel31: Kind = 42;
pub const got_prel: Kind = 96;

const Named = struct { kind: Kind, name: []const u8 };

const names = [_]Named{
    .{ .kind = none, .name = "R_ARM_NONE" },
    .{ .kind = abs32, .name = "R_ARM_ABS32" },
    .{ .kind = rel32, .name = "R_ARM_REL32" },
    .{ .kind = abs16, .name = "R_ARM_ABS16" },
    .{ .kind = abs12, .name = "R_ARM_ABS12" },
    .{ .kind = abs8, .name = "R_ARM_ABS8" },
    .{ .kind = sbrel32, .name = "R_ARM_SBREL32" },
    .{ .kind = gotoff32, .name = "R_ARM_GOTOFF32" },
    .{ .kind = base_prel, .name = "R_ARM_BASE_PREL" },
    .{ .kind = got_brel, .name = "R_ARM_GOT_BREL" },
    .{ .kind = target1, .name = "R_ARM_TARGET1" },
    .{ .kind = prel31, .name = "R_ARM_PREL31" },
    .{ .kind = got_prel, .name = "R_ARM_GOT_PREL" },
};

/// The ABI's name for `kind`, or null for one this table does not carry.
pub fn name(kind: Kind) ?[]const u8 {
    for (names) |entry| {
        if (entry.kind == kind) return entry.name;
    }
    return null;
}

/// True for the types whose stored value does not depend on where the
/// module is loaded: relative to the place, to the GOT or to the data base.
///
/// This is an allow-list. A type it does not name, known to the table above
/// or not, is treated as one the loader would have to rebase.
pub fn movesWithTheModule(kind: Kind) bool {
    return switch (kind) {
        none, rel32, sbrel32, gotoff32, base_prel, got_brel, prel31, got_prel => true,
        else => false,
    };
}
