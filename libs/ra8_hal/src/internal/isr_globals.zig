//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Global maskable-IRQ gate for ra8_isr (RA8FW-706). `Hw` supplies the
//! PRIMASK clear/set pair: inline cpsie/cpsid on target, the host stubs in
//! tests/mocks/src/ra8_host_asm_stub.c otherwise.

/// Clear PRIMASK so maskable IRQs may dispatch.
pub fn enable(comptime Hw: type) void {
    Hw.irqEnable();
}

/// Set PRIMASK so later maskable IRQs pend until re-enabled.
pub fn disable(comptime Hw: type) void {
    Hw.irqDisable();
}
