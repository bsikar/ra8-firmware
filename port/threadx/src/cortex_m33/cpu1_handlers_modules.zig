//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The three CPU1 system handlers `threadx_cpu1.zig` routes per kernel, for
//! `threadx_m33_modules` (RA8FW-431). The Module Manager port's
//! `tx_thread_schedule.S` defines all three: a module thread reaches the
//! kernel through SVC, and a module's MPU or bus fault is handed to
//! `_txm_module_manager_memory_fault_handler` instead of hanging the core.

extern fn MemManage_Handler() callconv(.c) void;
extern fn BusFault_Handler() callconv(.c) void;
extern fn SVC_Handler() callconv(.c) void;

pub const Handler = *const fn () callconv(.c) void;

pub const mem_manage: Handler = &MemManage_Handler;
pub const bus_fault: Handler = &BusFault_Handler;
pub const svc: Handler = &SVC_Handler;
