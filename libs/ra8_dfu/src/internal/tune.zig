//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Timing and retry tunables for the polled enumeration and the DFU status
//! poll. Separate from `proto.zig` because every value here is a choice this
//! driver makes, not something the USB or DFU specification fixes.

/// Delays, in milliseconds.
pub const Delay = struct {
    /// VBUS settle before the port is probed at all.
    pub const vbus_settle_ms: u32 = 200;
    /// Cap on waiting for the device's D+ pull-up.
    pub const attach_timeout_ms: u32 = 2000;
    /// Post-attach debounce.
    pub const debounce_ms: u32 = 500;
    /// Bus-reset hold. USB 2.0 wants at least 10 ms.
    pub const reset_hold_ms: u32 = 50;
    /// Post-reset recovery, TRSTRCY.
    pub const recovery_ms: u32 = 20;
    /// Post-SET_ADDRESS recovery before the DCP is retargeted.
    pub const address_settle_ms: u32 = 5;
    /// Pause between DFU_GETSTATUS polls.
    pub const status_poll_ms: u32 = 2;
};

/// Attempt counts.
pub const Retry = struct {
    /// GETSTATUS polls before a state wait is called a timeout.
    pub const status_tries: u32 = 50;
    /// Bus-reset-and-probe attempts before enumeration is called a timeout.
    pub const enum_tries: u8 = 8;
    /// Iteration cap on the attach spin, so a dead port cannot wedge the
    /// caller even if the millisecond clock is not advancing.
    pub const attach_spin: u32 = 50_000_000;
};
