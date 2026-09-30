//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! SysTick reload arithmetic: core clock + tick rate -> SYST_RVR value.
//!
//! No MMIO here on purpose. This is the whole decidable part of the driver,
//! so it is a plain function over integers that the Zig tests can drive
//! without a register window.

/// Hardware bounds of the SysTick reload field.
pub const limits = struct {
    /// SYST_RVR is 24 bits; a larger reload cannot be represented.
    pub const rvr_max: u32 = 0x00FF_FFFF;
};

pub const ReloadError = error{
    /// A 0 Hz core or a 0 Hz tick rate: the division is meaningless.
    ZeroRate,
    /// The core is slower than one tick period, so `ticks - 1` would wrap.
    ClockBelowTick,
    /// Representable as a tick count, but wider than the 24-bit field.
    OutOfRange,
};

/// SYST_RVR value that makes `cpu_hz` produce `tick_hz` ticks per second.
pub fn reloadFor(cpu_hz: u32, tick_hz: u32) ReloadError!u32 {
    if (cpu_hz == 0 or tick_hz == 0) return error.ZeroRate;

    const ticks = cpu_hz / tick_hz;
    if (ticks == 0) return error.ClockBelowTick;

    const reload = ticks - 1;
    if (reload > limits.rvr_max) return error.OutOfRange;
    return reload;
}

/// Whether a caller-supplied reload fits the field, for the entry points that
/// take one directly instead of computing it.
pub fn fits(reload: u32) bool {
    return reload <= limits.rvr_max;
}
