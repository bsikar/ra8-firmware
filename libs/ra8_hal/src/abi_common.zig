//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Shared by every ra8_hal unit's *_abi.zig: the `ra8_err_t` values
//! (libs/ra8_core/inc/ra8_err.h, a uint16_t enum) and the ra8_log entry
//! points. Declarations only, so it adds no code or strings to any object.

pub const k_ra8_ok: u16 = 0;
pub const k_ra8_err_invalid_arg: u16 = 0x103;
pub const k_ra8_err_invalid_size: u16 = 0x105;
pub const k_ra8_err_invalid_state: u16 = 0x104;
pub const k_ra8_err_not_supported: u16 = 0x107;
pub const k_ra8_err_exists: u16 = 0x10C;
pub const k_ra8_err_not_initialized: u16 = 0x10F;
pub const k_ra8_err_hw_timeout: u16 = 0x203;
pub const k_ra8_err_hw_error: u16 = 0x204;
pub const k_ra8_err_out_of_range: u16 = 0x208;
pub const k_ra8_err_no_data: u16 = 0x10A;
pub const k_ra8_err_null_ptr: u16 = 0x504;

pub extern fn ra8_log_emit_info(tag: [*:0]const u8, message: [*:0]const u8) void;
pub extern fn ra8_log_emit_info_val(tag: [*:0]const u8, message: [*:0]const u8, value: u32) void;
pub extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;
pub extern fn ra8_log_emit_error_val(tag: [*:0]const u8, message: [*:0]const u8, value: u32) void;
