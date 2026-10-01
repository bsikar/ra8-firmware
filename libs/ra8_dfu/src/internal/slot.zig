//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Which A/B slot wins at reset, and what the bootloader does about it.

const std = @import("std");

/// The slot a decision names. Values are the C `ra8_dfu_slot_t`.
pub const Slot = enum(u8) {
    a = 0,
    b = 1,
    none = 2,
};

/// What the bootloader does at reset. Values are the C `ra8_dfu_action_t`.
pub const Action = enum(u8) {
    dfu = 0,
    jump_a = 1,
    jump_b = 2,
};

/// One slot as the selection sees it: valid or not, and its sequence number.
pub const Candidate = struct {
    valid: bool,
    seq: u32,
};

/// The newer valid slot, A winning a tie. `.none` when neither is valid.
pub fn select(a: Candidate, b: Candidate) Slot {
    if (!a.valid and !b.valid) return .none;
    if (a.valid and (!b.valid or a.seq >= b.seq)) return .a;
    return .b;
}

/// The reset-time decision: the trigger wins outright, otherwise boot the
/// selected slot, and fall back to DFU when there is nothing to boot.
pub fn decide(dfu_trigger: bool, a: Candidate, b: Candidate) Action {
    if (dfu_trigger) return .dfu;
    return switch (select(a, b)) {
        .a => .jump_a,
        .b => .jump_b,
        .none => .dfu,
    };
}

test "a tie goes to slot A" {
    const tie = Candidate{ .valid = true, .seq = 4 };
    try std.testing.expectEqual(Slot.a, select(tie, tie));
}
