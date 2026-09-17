//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Watchdog-supervisor policy core: the registry shape, the deadline
//! arithmetic and the refresh verdict, with no ThreadX and no C ABI in sight.
//! `ra8_wdt_supervisor_abi.zig` owns the exported symbols, the module state and
//! the kernel seam; everything decidable without a kernel lives here so it can
//! be tested as plain values.

const std = @import("std");

/// Max simultaneously registered threads (`k_ra8_wdt_sup_max_threads`).
pub const max_threads: u8 = 8;
/// Max bytes, NUL included, kept for a thread name (`k_ra8_wdt_sup_name_max`).
pub const name_max: usize = 16;
/// Sentinel written to `*out_handle` on a failed registration.
pub const handle_invalid: u8 = 0xFF;
/// Smallest supervisor stack the config gate accepts, in bytes.
pub const min_stack_bytes: u32 = 512;
/// Highest legal ThreadX priority.
pub const max_priority: u32 = 31;
/// Default kernel tick, in milliseconds: a 1 kHz tick.
pub const default_tick_ms: u32 = 1;

/// Slot tag: empty.
pub const slot_free: u8 = 0;
/// Slot tag: registered.
pub const slot_used: u8 = 1;

/// One row of the registry. Module-private in the C too, so the layout is not
/// part of the ABI; it stays `extern` only to keep the field order readable.
pub const Slot = extern struct {
    state: u8 = slot_free,
    name: [name_max]u8 = [_]u8{0} ** name_max,
    deadline_ms: u32 = 0,
    last_checkin_ms: u32 = 0,
};

/// Which of the two config errors the gate reports.
pub const CfgFault = enum { null_ptr, invalid_arg };

/// The config fields the gate actually reads, lifted out of the C struct so the
/// gate is testable without a pointer.
pub const CfgView = struct {
    has_stack: bool,
    stack_size_bytes: u32,
    priority: u32,
    refresh_period_ms: u32,
};

/// Validate the public configuration block.
///
/// Guard order is the contract: a missing block and a missing stack both report
/// `null_ptr`, then stack size, then period, then priority.
pub fn validateCfg(cfg: ?CfgView) ?CfgFault {
    const view = cfg orelse return .null_ptr;
    if (!view.has_stack) return .null_ptr;
    if (view.stack_size_bytes < min_stack_bytes) return .invalid_arg;
    if (view.refresh_period_ms == 0) return .invalid_arg;
    if (view.priority > max_priority) return .invalid_arg;
    return null;
}

/// True when `now - last_checkin` has run past `deadline`.
///
/// Wrapping subtraction, exactly as the C's unsigned arithmetic: correct for
/// any real gap below 2^31 ms (~24.8 days).
pub fn isOverdue(now: u32, last_checkin: u32, deadline: u32) bool {
    const gap = now -% last_checkin;
    return gap > deadline;
}

/// Copy at most `name_max - 1` bytes of a NUL-terminated name, zero-filling the
/// rest so the stored name is always terminated.
pub fn copyName(dst: *[name_max]u8, name: [*:0]const u8) void {
    @memset(dst, 0);
    var k: usize = 0;
    while (k < name_max - 1) : (k += 1) {
        const ch = name[k];
        if (ch == 0) break;
        dst[k] = ch;
    }
}

/// What one supervisor tick concluded about the registry.
pub const Verdict = struct {
    any_present: bool,
    all_alive: bool,

    /// Refresh only when at least one thread is registered and every registered
    /// thread is inside its deadline. Zero workers deliberately does NOT kick
    /// the dog: the supervisor would otherwise mask a degenerate config.
    pub fn willRefresh(self: Verdict) bool {
        return self.any_present and self.all_alive;
    }
};

/// The statically-allocated check-in registry.
pub const Registry = extern struct {
    slots: [max_threads]Slot = [_]Slot{.{}} ** max_threads,

    /// Return every slot to the free state.
    pub fn clear(self: *Registry) void {
        self.* = .{};
    }

    /// Index of the first free slot, or null when the registry is full.
    pub fn findFree(self: *const Registry) ?u8 {
        var i: u8 = 0;
        while (i < max_threads) : (i += 1) {
            if (self.slots[i].state == slot_free) return i;
        }
        return null;
    }

    /// Populate a free slot, priming the check-in stamp to `now` so the first
    /// tick cannot call a freshly registered thread overdue.
    pub fn fill(self: *Registry, idx: u8, name: [*:0]const u8, deadline_ms: u32, now: u32) void {
        const slot = &self.slots[idx];
        slot.state = slot_used;
        slot.deadline_ms = deadline_ms;
        slot.last_checkin_ms = now;
        copyName(&slot.name, name);
    }

    /// True when `handle` names a registered slot.
    pub fn isRegistered(self: *const Registry, handle: u8) bool {
        if (handle >= max_threads) return false;
        return self.slots[handle].state == slot_used;
    }

    /// Number of registered slots.
    pub fn used(self: *const Registry) u8 {
        var count: u8 = 0;
        var i: u8 = 0;
        while (i < max_threads) : (i += 1) {
            if (self.slots[i].state == slot_used) count += 1;
        }
        return count;
    }

    /// Walk the registry once and decide whether this tick refreshes the WDT.
    /// Stops at the first overdue slot, as the C loop does.
    pub fn verdict(self: *const Registry, now: u32) Verdict {
        var result = Verdict{ .any_present = false, .all_alive = true };
        var i: u8 = 0;
        while (i < max_threads) : (i += 1) {
            const slot = self.slots[i];
            if (slot.state != slot_used) continue;
            result.any_present = true;
            if (isOverdue(now, slot.last_checkin_ms, slot.deadline_ms)) {
                result.all_alive = false;
                break;
            }
        }
        return result;
    }
};

comptime {
    std.debug.assert(@offsetOf(Slot, "state") == 0);
    std.debug.assert(@offsetOf(Slot, "name") == 1);
    std.debug.assert(name_max == 16);
    std.debug.assert(max_threads == 8);
}
