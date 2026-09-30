//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The Arm v8-M System Control Block register window, and nothing else.
//!
//! Every raw address the fault path and the trace gate touch lives here, so
//! the callers above read as a sequence of named register operations. The
//! host suites reach these same addresses through the fake MMIO map
//! (`tests/mocks/src/ra8_fake_mmap.c`), which is why the accessors are plain
//! volatile loads and stores rather than anything conditional on the target.
//!
//! Arm v8-M ARM B3.2 "System Control Block" and the Debug / Security
//! Extension register views. The RA8D2 hardware manual defers the Cortex-M85
//! core registers to that document, so the citations here do too.

/// Register addresses in the PPB window at 0xE000EDxx.
pub const addr = struct {
    /// VTOR, vector table offset.
    pub const vtor: usize = 0xE000_ED08;
    /// CFSR, configurable fault status (MMFSR | BFSR | UFSR).
    pub const cfsr: usize = 0xE000_ED28;
    /// HFSR, HardFault status.
    pub const hfsr: usize = 0xE000_ED2C;
    /// DFSR, debug fault status.
    pub const dfsr: usize = 0xE000_ED30;
    /// MMFAR, MemManage fault address.
    pub const mmfar: usize = 0xE000_ED34;
    /// BFAR, BusFault address.
    pub const bfar: usize = 0xE000_ED38;
    /// AFSR, auxiliary fault status.
    pub const afsr: usize = 0xE000_ED3C;
    /// SFSR, SecureFault status, banked to the Secure state.
    pub const sfsr: usize = 0xE000_EDE4;
    /// SFAR, SecureFault address, banked to the Secure state.
    pub const sfar: usize = 0xE000_EDE8;
    /// DEMCR, debug exception and monitor control.
    pub const demcr: usize = 0xE000_EDFC;
};

/// Single bits these registers define.
pub const bits = struct {
    /// DEMCR.TRCENA, bit 24: trace subsystem power.
    pub const demcr_trcena: u32 = 1 << 24;
};

/// The eight fault-status registers, in the order `ra8_scb_fault_status_t`
/// declares them. Note this is NOT the order the exception record uses.
pub const FaultStatus = extern struct {
    cfsr: u32,
    hfsr: u32,
    dfsr: u32,
    mmfar: u32,
    bfar: u32,
    afsr: u32,
    sfsr: u32,
    sfar: u32,
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

/// Plain reads with no side effect: nothing here clears a status bit.
///
/// SFSR and SFAR are banked to the Secure state. Read from Secure they carry
/// the real cause and address; read from Non-secure they are architecturally
/// RAZ and never fault, so the capture needs no world guard.
pub fn readFaultStatus() FaultStatus {
    return .{
        .cfsr = read(addr.cfsr),
        .hfsr = read(addr.hfsr),
        .dfsr = read(addr.dfsr),
        .mmfar = read(addr.mmfar),
        .bfar = read(addr.bfar),
        .afsr = read(addr.afsr),
        .sfsr = read(addr.sfsr),
        .sfar = read(addr.sfar),
    };
}

/// Point the core at a new vector-table base. Hardware ignores the low
/// alignment bits.
pub fn setVtor(base: usize) void {
    write(addr.vtor, @truncate(base));
}

pub fn getVtor() usize {
    return read(addr.vtor);
}

pub fn traceEnabled() bool {
    return read(addr.demcr) & bits.demcr_trcena != 0;
}

/// Read-modify-write, so every other DEMCR bit an owner already set survives.
pub fn traceEnable() void {
    write(addr.demcr, read(addr.demcr) | bits.demcr_trcena);
}
