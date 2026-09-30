//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The two runtime odds and ends the freestanding build still owes the
//! toolchain: integer `abs`, and the `errno` slot the ARM EABI's libm
//! reaches for.

/// Absolute value, with the one edge the C standard leaves undefined.
///
/// `abs(INT_MIN)` has no representable answer, and the C here computed
/// `-j` and let the target decide. On every two's-complement machine that
/// is `INT_MIN` again, so the wrapping negate below keeps exactly that
/// behaviour rather than trapping on it: a boot-time runtime primitive is
/// the wrong place to start panicking, and a caller relying on the old
/// value should not silently get a different one.
pub fn absolute(j: i32) i32 {
    return if (j < 0) -%j else j;
}

/// Storage behind `__errno`.
///
/// The ARM EABI libm writes through the pointer `__errno()` hands back, so
/// this has to be one stable location for the life of the image. Nothing
/// first-party reads it; it exists so a libm call cannot write to address
/// zero.
pub var errno_slot: i32 = 0;
