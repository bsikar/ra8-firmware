//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The three CPU1 system handlers `threadx_cpu1.zig` routes per kernel, for
//! `threadx_m33` (RA8FW-431). TX_SINGLE_MODE_SECURE has no SVC path and no
//! module faults, so all three spin in the port's `__tx_BadHandler`.

extern fn __tx_BadHandler() callconv(.c) void;

pub const Handler = *const fn () callconv(.c) void;

pub const mem_manage: Handler = &__tx_BadHandler;
pub const bus_fault: Handler = &__tx_BadHandler;
pub const svc: Handler = &__tx_BadHandler;
