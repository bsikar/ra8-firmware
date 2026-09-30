//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The SysTick and DWT register window, and nothing else.
//!
//! Every raw address in the timebase lives here so the driver above reads as
//! a sequence of named register operations. The host suites reach these same
//! addresses through the fake MMIO map (`tests/mocks/src/ra8_fake_mmap.c`),
//! which is why the accessors are plain volatile loads and stores rather
//! than anything conditional on the target.

/// Addresses in the System Control Space and the DWT unit.
pub const addr = struct {
    /// SYST_CSR control and status.
    pub const syst_csr: usize = 0xE000_E010;
    /// SYST_RVR reload value.
    pub const syst_rvr: usize = 0xE000_E014;
    /// SYST_CVR current value.
    pub const syst_cvr: usize = 0xE000_E018;
    /// DWT_CTRL, bit 0 CYCCNTENA.
    pub const dwt_ctrl: usize = 0xE000_1000;
    /// DWT_CYCCNT free-running cycle counter.
    pub const dwt_cyccnt: usize = 0xE000_1004;
};

/// Single bits these registers define.
pub const bits = struct {
    /// SYST_CSR.ENABLE.
    pub const csr_enable: u32 = 1 << 0;
    /// SYST_CSR.TICKINT.
    pub const csr_tickint: u32 = 1 << 1;
    /// SYST_CSR.CLKSOURCE, set for the processor clock.
    pub const csr_clksource: u32 = 1 << 2;
    /// DWT_CTRL.CYCCNTENA.
    pub const dwt_cyccntena: u32 = 1 << 0;
};

fn at(address: usize) *volatile u32 {
    return @ptrFromInt(address);
}

pub fn read(address: usize) u32 {
    return at(address).*;
}

pub fn write(address: usize, value: u32) void {
    at(address).* = value;
}

/// Read-modify-write, so trace or debug bits another owner already set survive.
pub fn setBits(address: usize, mask: u32) void {
    at(address).* |= mask;
}
