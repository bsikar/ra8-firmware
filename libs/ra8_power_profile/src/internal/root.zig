//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Accumulator core of the power profiler, with no C types and no hooks.
//! The ABI membrane in `../ra8_power_profile_abi.zig` owns the module-static
//! instance, the `ra8_err_t` mapping, and the GPIO/clock function pointers;
//! everything here is pure bookkeeping over timestamps the caller supplies.

const std = @import("std");

/// Capacity of the accumulator array (`k_ra8_power_profile_max_regions`).
pub const max_regions: u8 = 16;

/// Per-region accumulator, laid out exactly as
/// `ra8_power_profile_region_stats_t`.
pub const RegionStats = extern struct {
    entries: u64 = 0,
    exits: u64 = 0,
    total_time_us: u64 = 0,
    last_enter_us: u64 = 0,
    is_open: bool = false,
};

/// Aggregate snapshot, laid out exactly as `ra8_power_profile_stats_t`.
pub const Stats = extern struct {
    regions: [max_regions]RegionStats = @splat(.{}),
};

/// Outcome of `markExit`, mapped to `ra8_err_t` by the ABI membrane.
pub const ExitOutcome = enum {
    /// A matching `markEnter` was open and its span was accumulated.
    closed,
    /// No enter was outstanding: the exit is counted, nothing accumulated.
    unmatched,
};

/// Bookkeeping for one profiler instance.
pub const Profiler = struct {
    stats: Stats = .{},

    /// True when `region` addresses a slot inside the accumulator array.
    pub fn inRange(region: u8) bool {
        return region < max_regions;
    }

    /// Count an entry into `region` and stamp it with `now_us`.
    ///
    /// An enter over an already-open region overwrites the open timestamp,
    /// matching the documented C behaviour: the entries counter still moves,
    /// so the imbalance stays visible in the snapshot.
    pub fn markEnter(self: *Profiler, region: u8, now_us: u64) void {
        std.debug.assert(inRange(region));
        const slot = &self.stats.regions[region];
        slot.entries += 1;
        slot.last_enter_us = now_us;
        slot.is_open = true;
    }

    /// Count an exit from `region` and fold the closed span into the total.
    ///
    /// A non-monotonic clock (`now_us` before the open timestamp) contributes
    /// nothing rather than wrapping the accumulator, which is what the C
    /// implementation's `now >= last_enter_us` guard did.
    pub fn markExit(self: *Profiler, region: u8, now_us: u64) ExitOutcome {
        std.debug.assert(inRange(region));
        const slot = &self.stats.regions[region];
        slot.exits += 1;
        if (!slot.is_open) return .unmatched;
        if (now_us >= slot.last_enter_us) {
            slot.total_time_us += now_us - slot.last_enter_us;
        }
        slot.is_open = false;
        return .closed;
    }

    /// Drop every accumulator back to its initial value.
    pub fn reset(self: *Profiler) void {
        self.stats = .{};
    }
};
