//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Board-standard PLL1 clock tree bring-up. The register-level transition is
//! the CGC HAL's job; this file's job is publishing the two board-qualified
//! rates an application is allowed to see, so no concrete clock driver
//! escapes into application code.

const hal = @import("hal.zig");
const vocab = @import("vocab.zig");

/// The rates the standard EK-RA8D2 tree settles on, from `ra8_time_constants.h`.
pub const Rates = struct {
    /// CPUCLK0 = PLL1P/1.
    pub const cpuclk0_hz: u32 = 1_000_000_000;
    /// PCLKA = PLL1P/8.
    pub const pclka_hz: u32 = 125_000_000;
};

/// Bring PLL1 up and publish the board rates.
///
/// On failure `out_rates` is left untouched, so a caller that ignores the
/// error code still cannot read a half-written record.
pub fn init(out_rates: ?*hal.ClockRates) u32 {
    const out = out_rates orelse return vocab.Err.invalid_arg;
    const err = hal.ra8_cgc_init();
    if (err != vocab.Err.ok) return err;
    out.* = .{ .cpuclk0_hz = Rates.cpuclk0_hz, .pclka_hz = Rates.pclka_hz };
    return vocab.Err.ok;
}
