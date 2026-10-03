//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The poll pump: wait for HANDSHAKE, clock one transaction, classify what
//! came back and route it, until the outstanding wait is answered or the
//! budget runs out. The link is polled, never interrupt-driven, so every byte
//! in either direction moves inside a transaction this loop starts.
//!
//! The loop is generic over a `port`, so it runs on the real handle through
//! the C ABI and on a scripted model in host tests. A port provides:
//! `handshakeActive() bool`, `delayMs(u16)`, `tx() []u8`, `rx() []const u8`,
//! `takeStaged() ?Staged`, `transfer() bool` and `dispatch(frame.View) bool`.

const frame = @import("frame.zig");

/// Handshake and pacing budget, in milliseconds and attempts.
pub const Timing = struct {
    /// How long one transaction waits for HANDSHAKE before giving up.
    pub const hs_wait_ms: u16 = 200;
    /// Gap between two HANDSHAKE samples.
    pub const hs_poll_ms: u16 = 1;
    /// Consecutive handshake misses after which the co-processor is absent.
    pub const hs_giveup: u16 = 3;
    /// Pause between two transactions.
    pub const gap_ms: u16 = 2;
};

/// `ra8_c6link_stats_t`: counters for one pump run. The C ABI pins the layout.
pub const Stats = extern struct {
    transfers: u16 = 0,
    data: u16 = 0,
    idle: u16 = 0,
    bad_checksum: u16 = 0,
    malformed: u16 = 0,
    rpc_in: u16 = 0,
    events: u16 = 0,
    eth_in: u16 = 0,
    undecodable: u16 = 0,
    unrouted: u16 = 0,
    hs_timeouts: u16 = 0,
};

/// A payload the caller staged in the transmit buffer, waiting to be sealed.
pub const Staged = struct {
    if_type: u8,
    len: u16,
};

/// How a pump run ended.
pub const Outcome = enum {
    /// At least one transaction was clocked.
    ok,
    /// The transport refused a transaction; the run stopped there.
    bus_fault,
    /// No transaction was clocked at all.
    timeout,
};

/// Wait, bounded, for the co-processor to arm HANDSHAKE.
fn handshake(port: anytype) bool {
    var waited: u16 = 0;
    while (waited < Timing.hs_wait_ms) : (waited += Timing.hs_poll_ms) {
        if (port.handshakeActive()) return true;
        port.delayMs(Timing.hs_poll_ms);
    }
    return false;
}

/// Seal the staged payload, or send the idle filler when nothing is staged.
fn stage(port: anytype) void {
    if (port.takeStaged()) |staged| {
        _ = frame.seal(port.tx(), staged.if_type, 0, staged.len);
    } else {
        frame.filler(port.tx());
    }
}

/// Count one classified frame and route it if it is real.
///
/// Classifies first and routes second, so a frame that failed its integrity
/// check reaches no consumer. Returns true when the outstanding wait was
/// satisfied and the pump should stop.
fn receive(port: anytype, stats: *Stats) bool {
    switch (frame.classify(port.rx())) {
        .idle => stats.idle +%= 1,
        .malformed => stats.malformed +%= 1,
        .bad_checksum => stats.bad_checksum +%= 1,
        .data => |view| {
            stats.data +%= 1;
            return port.dispatch(view);
        },
    }
    return false;
}

/// Clock up to `max_transactions` transactions through `port`.
///
/// Stops early when a dispatched frame answers the outstanding wait, when the
/// transport faults, or after `Timing.hs_giveup` handshake misses in a row: a
/// co-processor that never arms the line is not there, and spending the whole
/// budget re-asking would turn an unplugged harness into a long hang.
pub fn run(port: anytype, max_transactions: u16, stats: *Stats) Outcome {
    var misses: u16 = 0;
    var i: u16 = 0;
    while (i < max_transactions) : (i += 1) {
        if (!handshake(port)) {
            stats.hs_timeouts +%= 1;
            misses += 1;
            if (misses >= Timing.hs_giveup) break;
            continue;
        }
        misses = 0;
        stage(port);
        if (!port.transfer()) return .bus_fault;
        stats.transfers +%= 1;
        if (receive(port, stats)) break;
        port.delayMs(Timing.gap_ms);
    }
    return if (stats.transfers == 0) .timeout else .ok;
}
