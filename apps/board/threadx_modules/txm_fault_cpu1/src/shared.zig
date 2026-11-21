//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The block both cores of txm_fault_cpu1 agree on, at the shared-SRAM address
//! txm_manager_cpu1 uses. CPU1's Module Manager thread and its fault callback
//! write it; the M85 only reads it after clearing it before the release. The
//! module's illegal store goes just past it (txm_fault_m33.outside_address).

pub const address: usize = 0x2210_0000;
/// "TXMF": CPU1's Module Manager thread is running.
pub const signature: u32 = 0x5458_4D46;
/// Manager ticks after the fault before the M85 calls it a PASS: proof the
/// kernel outlived the module.
pub const pass_ticks: u32 = 10;

/// The manager step that failed, if any.
pub const Step = struct {
    pub const none: u32 = 0;
    pub const initialize: u32 = 1;
    pub const object_pool: u32 = 2;
    pub const notify: u32 = 3;
    pub const load: u32 = 4;
    pub const start: u32 = 5;
};

pub const Block = extern struct {
    signature: u32,
    /// A `Step` value; `Step.none` while every call has succeeded.
    failed_step: u32,
    /// The ThreadX status the failed step returned.
    result: u32,
    /// Memory faults the manager has reported through the notify callback.
    faults: u32,
    /// 1 when the last fault named this app's module instance.
    own_module: u32,
    /// CFSR and MMFAR as the manager's MemManage handler saved them.
    cfsr: u32,
    mmfar: u32,
    /// Manager ticks counted since the first fault.
    ticks_after_fault: u32,
};

pub fn block() *volatile Block {
    return @ptrFromInt(address);
}
