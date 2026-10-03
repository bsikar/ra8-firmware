//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The block both cores of txm_table_cpu1 agree on, in shared SRAM at the
//! address threadx_cpu1 already uses (SRAM1 upper half, unused with TrustZone
//! off). CPU1 writes it, from its Module Manager thread and from the
//! module's application requests; the M85 only reads it after clearing it
//! before the release.

pub const address: usize = 0x2210_0000;
/// "TXM3": CPU1's Module Manager thread is running.
pub const signature: u32 = 0x5458_4D33;
/// How many values the module has to report, all of them right, for a PASS.
pub const pass_reports: u32 = 10;
/// How many of the first values the block keeps for the verdict line.
pub const kept = 4;

/// The module starts its counter at five, doubles it on even steps and
/// squares it on odd ones. These are the first four results, which the
/// module can only produce by calling through its table.
pub const first_values = [kept]u32{ 10, 100, 200, 40000 };

/// The manager step that failed, if any.
pub const Step = struct {
    pub const none: u32 = 0;
    pub const initialize: u32 = 1;
    pub const object_pool: u32 = 2;
    pub const load: u32 = 3;
    pub const start: u32 = 4;
    /// The module itself reported a failure; `result` is its stage.
    pub const module: u32 = 5;
};

pub const Block = extern struct {
    signature: u32,
    /// A `Step` value; `Step.none` while every call has succeeded.
    failed_step: u32,
    /// The ThreadX status the failed step returned.
    result: u32,
    /// Values the module has reported.
    reports: u32,
    /// Reported values that were not the next one expected.
    mismatches: u32,
    /// The first `kept` values, as reported.
    values: [kept]u32,
};

pub fn block() *volatile Block {
    return @ptrFromInt(address);
}
