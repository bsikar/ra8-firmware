//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Pure low-battery nag policy: the edge-triggered band detector with
//! hysteresis that `libs/ra8_batt/inc/ra8_batt.h` documents, with no C ABI
//! and no logging in sight. A caller folds one state-of-charge reading in
//! per tick and gets back the warning to surface, if any.
//!
//! The step is a single forward pass: no loop, no recursion, no allocation
//! (NASA P10 rules 1-3), so it behaves identically on the host harness and
//! on the RA8D2.

const std = @import("std");

/// Low-battery threshold, inclusive (`k_ra8_batt_low_pct`).
pub const low_pct: u8 = 20;
/// Critical threshold, inclusive (`k_ra8_batt_critical_pct`).
pub const critical_pct: u8 = 10;
/// Hysteresis above a band needed to re-arm it (`k_ra8_batt_rearm_margin`).
pub const rearm_margin: u8 = 3;
/// State-of-charge clamp ceiling (`k_ra8_batt_pct_max`).
pub const pct_max: u8 = 100;

/// SOC must rise strictly above this for the low band to re-arm.
pub const low_rearm_pct: u8 = low_pct + rearm_margin;
/// SOC must rise strictly above this for the critical band to re-arm.
pub const critical_rearm_pct: u8 = critical_pct + rearm_margin;

/// Warning a single step raises (`ra8_batt_nag_t`).
pub const Nag = enum(u8) {
    none = 0,
    low = 1,
    critical = 2,
};

/// Caller-owned nag state carried across steps (`ra8_batt_monitor_t`).
///
/// A flag is true only between its band's nag and its re-arm.
pub const Monitor = extern struct {
    low_raised: bool,
    critical_raised: bool,

    /// Reset to the un-nagged, fully-armed state.
    pub fn reset(self: *Monitor) void {
        self.low_raised = false;
        self.critical_raised = false;
    }
};

/// Clamp a reported percent into `0..=pct_max`.
///
/// The C did this with a ternary, so an over-range reading reads as full
/// rather than being rejected.
pub fn clampSoc(soc_pct: u8) u8 {
    return if (soc_pct > pct_max) pct_max else soc_pct;
}

/// Whether a band re-arms this step: charging, or SOC recovered strictly
/// past the band's margin.
///
/// This is the `charging || (soc > rearm)` decision the MC/DC vectors in
/// `tests/misc/src/test_ra8_batt.c` pin, kept as one named predicate so both
/// bands share exactly one implementation.
pub fn rearms(charging: bool, soc: u8, rearm_above_pct: u8) bool {
    return charging or (soc > rearm_above_pct);
}

/// Whether a band fires this step: inside the band and not already raised.
pub fn raises(soc: u8, threshold_pct: u8, already_raised: bool) bool {
    return (soc <= threshold_pct) and !already_raised;
}

/// Fold one reading into `mon` and report the warning to surface.
///
/// Re-arm runs first (so a recovery in the same step that dips back can
/// warn again), then, only while not charging, the two bands raise. A drop
/// that enters both bands at once reports the worse of the two, which is
/// why the critical arm is checked second and overwrites.
pub fn step(mon: *Monitor, soc_pct: u8, charging: bool) Nag {
    const soc = clampSoc(soc_pct);

    if (rearms(charging, soc, low_rearm_pct)) {
        mon.low_raised = false;
    }
    if (rearms(charging, soc, critical_rearm_pct)) {
        mon.critical_raised = false;
    }

    var nag: Nag = .none;
    if (!charging) {
        if (raises(soc, low_pct, mon.low_raised)) {
            mon.low_raised = true;
            nag = .low;
        }
        if (raises(soc, critical_pct, mon.critical_raised)) {
            mon.critical_raised = true;
            nag = .critical;
        }
    }
    return nag;
}

/// Short, stable upper-case label for a nag value.
///
/// Takes the raw byte rather than `Nag`, because the C contract documents a
/// `"?"` answer for an out-of-range enumerator and the host suite passes
/// 200 to check it.
pub fn label(nag: u8) [*:0]const u8 {
    return switch (nag) {
        @intFromEnum(Nag.none) => "OK",
        @intFromEnum(Nag.low) => "LOW",
        @intFromEnum(Nag.critical) => "CRITICAL",
        else => "?",
    };
}

comptime {
    // The monitor is two packed booleans in the C, and consumers embed it by
    // value, so its shape is part of the ABI.
    std.debug.assert(@sizeOf(Monitor) == 2);
    std.debug.assert(@offsetOf(Monitor, "low_raised") == 0);
    std.debug.assert(@offsetOf(Monitor, "critical_raised") == 1);
    // Thresholds are a public enum in the header; a drift here is a contract
    // break, not an implementation detail.
    std.debug.assert(low_rearm_pct == 23);
    std.debug.assert(critical_rearm_pct == 13);
}
