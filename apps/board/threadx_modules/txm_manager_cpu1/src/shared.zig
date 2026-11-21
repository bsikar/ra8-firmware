//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The block both cores of txm_manager_cpu1 agree on, in shared SRAM at the
//! address threadx_cpu1 already uses (SRAM1 upper half, unused with TrustZone
//! off). CPU1's Module Manager thread writes it; the M85 only reads it after
//! clearing it before the release.

pub const address: usize = 0x2210_0000;
/// "TXM1": CPU1's Module Manager thread is running.
pub const signature: u32 = 0x5458_4D31;
/// How many times the module's start thread has to have run for a PASS.
pub const pass_runs: u32 = 10;

/// The manager step that failed, if any.
pub const Step = struct {
    pub const none: u32 = 0;
    pub const initialize: u32 = 1;
    pub const object_pool: u32 = 2;
    pub const load: u32 = 3;
    pub const start: u32 = 4;
};

pub const Block = extern struct {
    signature: u32,
    /// A `Step` value; `Step.none` while every call has succeeded.
    failed_step: u32,
    /// The ThreadX status the failed step returned.
    result: u32,
    /// The module start thread's run count, copied once per tick.
    module_runs: u32,
};

pub fn block() *volatile Block {
    return @ptrFromInt(address);
}
