//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Input capture on GTCCRA for the timer adapter (RA8FW-299, dev f00c0bbb0).
//!
//! A channel opened through `fw_timer_ra8_open_capture` is armed: GTCCRA
//! latches GTCNT on the routed edge sources instead of comparing. GTST.TCFA
//! is sticky per edge, so the first read after an edge folds it into a
//! per-channel `latched` flag and clears it; reads before any edge since the
//! open answer `would_block` rather than a stale register.

const Err = @import("err").Err;
const claim = @import("claim");
const hal = @import("gpt_hal");

const State = struct {
    armed: bool = false,
    latched: bool = false,
};

var states: [claim.channel_count]State = @splat(.{});

/// Whether `source_mask` names at least one source and only legal ones.
pub fn validSources(source_mask: u32) bool {
    return source_mask != 0 and (source_mask & ~hal.CapSrc.valid_mask) == 0;
}

/// Route `source_mask` into GTCCRA on an already-open channel and arm it.
pub fn arm(channel: u8, source_mask: u32) u16 {
    const err = hal.ra8_gpt_capture_configure(channel, hal.Ccr.a, source_mask);
    if (err == Err.ok) states[channel] = .{ .armed = true };
    return err;
}

/// Return GTCCRA to compare use if armed, and forget the channel's state.
pub fn disarm(channel: u8) void {
    if (states[channel].armed) {
        _ = hal.ra8_gpt_capture_configure(channel, hal.Ccr.a, hal.CapSrc.none);
    }
    states[channel] = .{};
}

/// The latched count, once an edge has landed since `arm`.
pub fn read(channel: u8, out_counts: ?*u32) u16 {
    if (!states[channel].armed) return Err.invalid_state;

    var status: u32 = 0;
    const err = hal.ra8_gpt_get_status(channel, &status);
    if (err != Err.ok) return err;

    if ((status & hal.Status.ccra) != 0) {
        states[channel].latched = true;
        _ = hal.ra8_gpt_clear_status(channel, hal.Status.ccra);
    }
    if (!states[channel].latched) return Err.would_block;
    return hal.ra8_gpt_capture_read(channel, hal.Ccr.a, out_counts);
}
