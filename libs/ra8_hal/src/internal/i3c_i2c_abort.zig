//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! I3C legacy-I2C controller abort (RA8FW-693): mask the bus and
//! normal-transfer interrupts, issue STOP, scrub BST and drop the held
//! bus. Generic over the bus helpers so host tests can record it.

/// HUM 40.2 BIE, p 2484.
pub const off_bie: usize = 0x1D8;
/// HUM 40.2 NTIE, p 2488.
pub const off_ntie: usize = 0x1E8;

/// `bus` provides stop and clearBst, the promoted `priv_i3c_i2c_*`
/// helpers. Interrupts are masked first, mirroring the FSP abort path.
pub fn run(bus: anytype, bie: *volatile u32, ntie: *volatile u32, bus_held: *bool) void {
    bie.* = 0;
    ntie.* = 0;
    bus.stop();
    bus.clearBst();
    bus_held.* = false;
}
