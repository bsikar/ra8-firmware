//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! SRAM TrustZone attribution leaves and the ECC error fan-out
//! (inc/ra8_sram.h, RA8FW-554). The three setters are gate-free leaves
//! whose caller holds PRCR PRC4 open (ra8_sram.h @pre); they are named in
//! scripts/checks/check_attribution_gates.py ALLOWED_LEAVES.

pub const bank_count: usize = 4;
/// k_ra8_sram_sar_writable: union of the defined SRAMSAR bits.
pub const sar_writable: u32 = 0x0000_010F;
/// k_ra8_sram_esar_bit_esa: ECC region Non-Secure.
pub const esar_esa: u32 = 0x0000_0001;
/// k_ra8_sram_sabar_align_mask: b12..b0 must be 0.
pub const sabar_align_mask: u32 = 0x0000_1FFF;
/// k_ra8_sram_cpscu_base_addr.
pub const cpscu_base: usize = 0x4000_8000;

pub const Error = error{InvalidArg};

/// `r_sram_cpscu_regs_t`.
pub const Cpscu = extern struct {
    _r0: [0x10]u8,
    SRAMSAR: u32,
    _r1: [0x3EC]u8,
    SRAMSABAR: [bank_count]u32,
    _r2: [0x100]u8,
    SRAMESAR: u32,
    _r3: u32,
};

/// `ra8_sram_status_t`.
pub const Status = extern struct {
    raw_esr: u16 = 0,
    one_bit_mask: u8 = 0,
    two_bit_mask: u8 = 0,
    addr_1bit: [bank_count]usize = @splat(0),
    addr_2bit: [bank_count]usize = @splat(0),
};

comptime {
    if (@offsetOf(Cpscu, "SRAMSAR") != 0x010) @compileError("SRAMSAR offset");
    if (@offsetOf(Cpscu, "SRAMSABAR") != 0x400) @compileError("SRAMSABAR offset");
    if (@offsetOf(Cpscu, "SRAMESAR") != 0x510) @compileError("SRAMESAR offset");
    if (@sizeOf(Cpscu) != 0x518) @compileError("Cpscu size");
    if (@offsetOf(Status, "addr_1bit") != @sizeOf(usize)) @compileError("Status layout");
}

/// SRAMSAR (HUM Ch 58.2.2 p 3528). Leaf: caller holds PRC4.
pub fn setSecurity(cpscu: *volatile Cpscu, sa_mask: u32) Error!void {
    if (sa_mask & ~sar_writable != 0) return error.InvalidArg;
    cpscu.SRAMSAR = sa_mask;
}

/// SRAMESAR (HUM Ch 58.2.3 p 3529). Leaf: caller holds PRC4.
pub fn setEccSecurity(cpscu: *volatile Cpscu, non_secure: bool) void {
    cpscu.SRAMESAR = if (non_secure) esar_esa else 0;
}

/// SRAMSABARn absolute Secure offset (HUM Ch 58.2.1 p 3527). Leaf: caller holds PRC4.
pub fn setBoundary(cpscu: *volatile Cpscu, bank: u8, offset: u32) Error!void {
    if (bank >= bank_count) return error.InvalidArg;
    if (offset & sabar_align_mask != 0) return error.InvalidArg;
    cpscu.SRAMSABAR[bank] = offset;
}

/// Fire `sink.fire(bank, is_2bit, addr)` per SRAMESR bit, 1-bit before
/// 2-bit within a bank; return the mask of bits that fired.
pub fn dispatchEsr(status: *const Status, sink: anytype) u16 {
    var fired: u16 = 0;
    for (0..bank_count) |b| {
        const bank: u8 = @intCast(b);
        const one: u16 = @as(u16, 1) << @intCast(2 * b);
        const two: u16 = one << 1;
        if (status.raw_esr & one != 0) {
            sink.fire(bank, false, status.addr_1bit[b]);
            fired |= one;
        }
        if (status.raw_esr & two != 0) {
            sink.fire(bank, true, status.addr_2bit[b]);
            fired |= two;
        }
    }
    return fired;
}
