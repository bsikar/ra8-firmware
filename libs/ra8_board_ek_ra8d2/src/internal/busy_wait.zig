//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! A bounded nop spin, for the two Ethernet settle delays that happen before
//! any timer this board owns is running: the PHY's reset pulse and the ETHA
//! mode-change dwell. Iteration counts are the ones the C carried; both are
//! specified as minimum wall-clock times, so overshooting is harmless and
//! undershooting is not.

/// Spin `iters` times, defeating the optimiser so the loop survives a release
/// build. The C used a `volatile` induction variable plus an inline `nop`;
/// the nop is what actually guarantees the loop body costs something.
pub fn spin(iters: u32) void {
    var i: u32 = 0;
    while (i < iters) : (i += 1) {
        asm volatile ("nop");
    }
}
