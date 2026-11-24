//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Canonical `ra8_err_t` constants and error mapping for `libs/ra8_translate`.

/// Numeric status codes matching `libs/ra8_core/inc/ra8_err.h`.
pub const Ra8Err = u16;

pub const k_ra8_ok: Ra8Err = 0;
pub const k_ra8_fail: Ra8Err = 0x101;
pub const k_ra8_err_no_mem: Ra8Err = 0x102;
pub const k_ra8_err_invalid_arg: Ra8Err = 0x103;
pub const k_ra8_err_invalid_state: Ra8Err = 0x104;
pub const k_ra8_err_invalid_size: Ra8Err = 0x105;
pub const k_ra8_err_not_found: Ra8Err = 0x106;
pub const k_ra8_err_not_supported: Ra8Err = 0x107;
pub const k_ra8_err_crc_mismatch: Ra8Err = 0x405;
pub const k_ra8_err_validation_failed: Ra8Err = 0x501;
pub const k_ra8_err_range_check_failed: Ra8Err = 0x503;
pub const k_ra8_err_null_ptr: Ra8Err = 0x504;

/// Recoverable domain error set inside `libs/ra8_translate`.
pub const TranslateError = error{
    InvalidArg,
    InvalidSize,
    NotFound,
    NotSupported,
    CrcMismatch,
    ValidationFailed,
    OutOfRange,
};

/// Map every internal translation error deterministically to `ra8_err_t`.
pub fn toRa8Err(e: TranslateError) Ra8Err {
    return switch (e) {
        error.InvalidArg => k_ra8_err_invalid_arg,
        error.InvalidSize => k_ra8_err_invalid_size,
        error.NotFound => k_ra8_err_not_found,
        error.NotSupported => k_ra8_err_not_supported,
        error.CrcMismatch => k_ra8_err_crc_mismatch,
        error.ValidationFailed => k_ra8_err_validation_failed,
        error.OutOfRange => k_ra8_err_range_check_failed,
    };
}
