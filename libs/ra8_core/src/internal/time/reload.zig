//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The 1 kHz reload arithmetic, and nothing else.
//!
//! One deliberate difference from the C this replaces. The C computed
//! `cpu_hz / 1000 - 1` and then rejected a zero reload, which caught a clock
//! in [1000, 1999]. A clock in [1, 999] divided to zero and the subtraction
//! wrapped to 0xFFFFFFFF, so the check passed and the caller got `k_ra8_ok`
//! with a reload no SysTick can hold. This rejects every clock below two tick
//! periods outright, which is what the header already promises.

/// Rates the counter is built around.
pub const limits = struct {
    /// Tick rate, in Hz.
    pub const tick_hz: u32 = 1000;
    /// Lowest CPU clock that yields a reload of at least one, in Hz.
    pub const min_cpu_hz: u32 = 2 * tick_hz;
};

pub const Error = error{
    /// The clock is zero, so there is no rate to divide.
    ZeroClock,
    /// The clock is below two tick periods, so the reload would be zero.
    ClockTooLow,
};

/// SysTick reload for one millisecond: the cycles in a tick, less the one the
/// reload itself consumes.
pub fn reloadFor(cpu_hz: u32) Error!u32 {
    if (cpu_hz == 0) return error.ZeroClock;
    if (cpu_hz < limits.min_cpu_hz) return error.ClockTooLow;
    return (cpu_hz / limits.tick_hz) - 1;
}

/// CPU cycles in one millisecond: the conversion the cycle-counter delay needs.
pub fn cyclesPerMs(cpu_hz: u32) u32 {
    return cpu_hz / limits.tick_hz;
}
