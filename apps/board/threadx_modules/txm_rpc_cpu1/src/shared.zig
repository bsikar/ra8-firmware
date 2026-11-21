//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The block both cores of txm_rpc_cpu1 agree on, in shared SRAM at the
//! address threadx_cpu1 already uses (SRAM1 upper half, unused with TrustZone
//! off). CPU1 writes it, from its Module Manager thread and from the
//! module's application requests; the M85 only reads it after clearing it
//! before the release.

pub const address: usize = 0x2210_0000;
/// "TXR4": CPU1's Module Manager thread is running.
pub const signature: u32 = 0x5458_5234;
/// How many sums the module has to report, all of them right, for a PASS.
pub const pass_reports: u32 = 10;
/// How many of the first sums the block keeps for the verdict line.
pub const kept = 4;

/// The first four sums: `service.sumFor` of steps 0 to 3. Each one is a
/// call the module made through ra8_rpc and the resident image answered.
pub const first_values = [kept]u32{ 101, 202, 303, 404 };

/// The step that failed, if any.
pub const Step = struct {
    pub const none: u32 = 0;
    pub const initialize: u32 = 1;
    pub const object_pool: u32 = 2;
    pub const load: u32 = 3;
    pub const start: u32 = 4;
    /// The module reported a failure; `result` is its `service.Stage`.
    pub const module: u32 = 5;
    /// The resident server failed; `result` is its error.
    pub const server: u32 = 6;
    /// The module never attached its queues.
    pub const attach: u32 = 7;
};

pub const Block = extern struct {
    signature: u32,
    /// A `Step` value; `Step.none` while everything has succeeded.
    failed_step: u32,
    /// The status or stage of the failed step.
    result: u32,
    /// Sums the module has reported.
    reports: u32,
    /// Reported sums that were not the next one expected.
    mismatches: u32,
    /// The first `kept` sums, as reported.
    values: [kept]u32,
    /// Calls the resident server answered.
    answered: u32,
};

pub fn block() *volatile Block {
    return @ptrFromInt(address);
}
