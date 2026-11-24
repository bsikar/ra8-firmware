//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Which A/B slot wins at reset, and what the bootloader does about it
//! (including trial-boot budget consumption and fallback on exhaustion).

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

/// Summary of the durable trial boot-state record as seen by the boot policy.
pub const TrialState = struct {
    trial_slot: Slot,
    tries_left: u8,
    confirmed: bool,
};

/// Complete trial-aware reset boot plan (`ra8_dfu_boot_plan_t` in `ra8_dfu.h`).
pub const BootPlan = struct {
    action: Action,
    is_trial: bool,
    next_tries: u8,
    dfu_target: Slot,
};

/// Map a concrete slot to its jump action (`.none` maps to `.dfu`).
pub fn jumpAction(which: Slot) Action {
    return switch (which) {
        .a => .jump_a,
        .b => .jump_b,
        .none => .dfu,
    };
}

/// The newer valid slot, A winning a tie. `.none` when neither is valid.
pub fn select(a: Candidate, b: Candidate) Slot {
    if (!a.valid and !b.valid) return .none;
    if (a.valid and (!b.valid or a.seq >= b.seq)) return .a;
    return .b;
}

/// Trial-aware reset-time boot decision:
/// - `dfu_trigger` wins outright -> `.dfu` targeting the inactive slot (or `.a`).
/// - If neither slot is valid -> `.dfu` targeting `.a`.
/// - Otherwise pick `preferred = select(a, b)`.
/// - If `state` tracks `preferred` as unconfirmed (`!confirmed`):
///   - When `tries_left > 0`, boot `preferred` as a trial with `next_tries = tries_left - 1`.
///   - When `tries_left == 0`, revert to the other slot if valid (non-trial), else `.dfu`.
/// - Confirmed (or untracked) slots keep higher-sequence-wins (`is_trial = false`).
pub fn decideWithState(
    dfu_trigger: bool,
    a: Candidate,
    b: Candidate,
    state: ?TrialState,
) BootPlan {
    const preferred = select(a, b);
    const other: Slot = if (preferred == .a) .b else .a;
    const other_valid: bool = switch (preferred) {
        .a => b.valid,
        .b => a.valid,
        .none => false,
    };

    if (dfu_trigger or preferred == .none) {
        return .{
            .action = .dfu,
            .is_trial = false,
            .next_tries = 0,
            .dfu_target = if (preferred == .none) .a else other,
        };
    }

    const on_trial = if (state) |s| s.trial_slot == preferred and !s.confirmed else false;
    if (!on_trial) {
        return .{
            .action = jumpAction(preferred),
            .is_trial = false,
            .next_tries = 0,
            .dfu_target = other,
        };
    }

    const tries = state.?.tries_left;
    if (tries > 0) {
        return .{
            .action = jumpAction(preferred),
            .is_trial = true,
            .next_tries = tries - 1,
            .dfu_target = other,
        };
    }

    if (other_valid) {
        return .{
            .action = jumpAction(other),
            .is_trial = false,
            .next_tries = 0,
            .dfu_target = preferred,
        };
    }

    return .{
        .action = .dfu,
        .is_trial = false,
        .next_tries = 0,
        .dfu_target = preferred,
    };
}

/// The legacy reset-time decision: trigger wins outright, otherwise higher-seq valid slot.
pub fn decide(dfu_trigger: bool, a: Candidate, b: Candidate) Action {
    return decideWithState(dfu_trigger, a, b, null).action;
}

test "a tie goes to slot A" {
    const tie = Candidate{ .valid = true, .seq = 4 };
    try std.testing.expectEqual(Slot.a, select(tie, tie));
}
