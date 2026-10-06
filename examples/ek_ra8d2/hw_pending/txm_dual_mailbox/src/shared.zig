//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The mailbox block both cores of txm_dual_mailbox agree on, in shared SRAM
//! at the address threadx_cpu1 already uses (SRAM1 upper half, unused with
//! TrustZone off). The M85 clears it before the release. CPU1 writes the status
//! words; the two slots carry queue messages each way (pump.zig).

pub const address: usize = 0x2210_0000;
/// "TXD1": CPU1's Module Manager thread is running.
pub const signature: u32 = 0x5458_4431;
/// Runs of each core's module start thread that count as a pass.
pub const pass_runs: u32 = 10;
/// Checked `add` round trips through the mailbox that count as a pass.
pub const pass_calls: u32 = 10;
/// One ThreadX queue message: sixteen 32-bit words (`service.message_words`).
pub const message_words = 16;

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
    /// Creating or binding one of CPU1's two server queues failed.
    pub const queue: u32 = 6;
    /// CPU1's `ra8_rpc` server failed; `result` is its error code.
    pub const server: u32 = 7;
};

/// One direction of the mailbox: one queue message in flight at a time.
pub const Slot = extern struct {
    /// Bumped by the sending core once `words` holds a new message.
    seq: u32 = 0,
    /// Set to `seq` by the receiving core once it has taken the message.
    ack: u32 = 0,
    words: [message_words]u32 = [_]u32{0} ** message_words,
};

pub const Block = extern struct {
    signature: u32 = 0,
    /// A `Step` value; `Step.none` while every call has succeeded.
    failed_step: u32 = Step.none,
    /// The ThreadX status the failed step returned.
    result: u32 = 0,
    /// CPU1's module start thread run count, copied once per tick.
    module_runs: u32 = 0,
    /// M85 to CPU1: the module's calls.
    request: Slot = .{},
    /// CPU1 to M85: the server's answers.
    reply: Slot = .{},
    /// Calls CPU1's server answered.
    answered: u32 = 0,
};

pub fn block() *volatile Block {
    return @ptrFromInt(address);
}

/// The word at `offset` in a module instance.
pub fn instanceWord(instance: []const u8, offset: usize) u32 {
    const word: *align(1) const volatile u32 = @ptrCast(&instance[offset]);
    return word.*;
}
