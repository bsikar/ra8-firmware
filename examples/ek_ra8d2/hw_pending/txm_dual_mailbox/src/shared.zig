//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The mailbox block both cores of txm_dual_mailbox agree on, in shared SRAM
//! at the address threadx_cpu1 already uses (SRAM1 upper half, unused with
//! TrustZone off). CPU1's Module Manager thread writes it; the M85 clears it
//! before the release and only reads it after.

pub const address: usize = 0x2210_0000;
/// "TXD1": CPU1's Module Manager thread is running.
pub const signature: u32 = 0x5458_4431;
/// Runs of each core's module start thread that count as a pass.
pub const pass_runs: u32 = 10;

/// offsetof(TXM_MODULE_INSTANCE, txm_module_instance_start_stop_thread) in
/// the M85's threadx_m85_modules build (RA8FW-825).
pub const m85_start_thread_offset = 0xC0;
/// The same offset in CPU1's threadx_m33_modules build, which lays the
/// instance out differently (measured in the emulator: TX_THREAD_ID sits at
/// +0xD0, as txm_manager_cpu1 already reads it).
pub const cpu1_start_thread_offset = 0xD0;
/// offsetof(TX_THREAD, tx_thread_run_count), past the start thread offset.
pub const run_count_offset = 4;
/// TX_THREAD_ID, the first word of a created TX_THREAD.
pub const tx_thread_id: u32 = 0x5448_5244;

/// The CPU1 manager step that failed, if any.
pub const Step = struct {
    pub const none: u32 = 0;
    pub const initialize: u32 = 1;
    pub const object_pool: u32 = 2;
    pub const load: u32 = 3;
    pub const start: u32 = 4;
    /// The start thread was not at `cpu1_start_thread_offset`.
    pub const thread: u32 = 5;
};

pub const Block = extern struct {
    signature: u32,
    /// A `Step` value; `Step.none` while every call has succeeded.
    failed_step: u32,
    /// The ThreadX status the failed step returned.
    result: u32,
    /// CPU1's module start thread run count, copied once per tick.
    module_runs: u32,
};

pub fn block() *volatile Block {
    return @ptrFromInt(address);
}

/// The word at `offset` in a module instance.
pub fn instanceWord(instance: []const u8, offset: usize) u32 {
    const word: *align(1) const volatile u32 = @ptrCast(&instance[offset]);
    return word.*;
}
