//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Routing J1 to the graphics controller.

const glcdc_pins = @import("glcdc_pins.zig");
const hal = @import("hal.zig");
const vocab = @import("vocab.zig");

const Err = vocab.Err;
const Pfs = vocab.Pfs;
const Psel = vocab.Psel;

/// `ra8_pfs_route_peripheral` writes PSEL and PMR but leaves PDR clear, which
/// keeps the pin an input under peripheral control. GLCDC has to drive, so
/// the three fields go down in one write under the PWPR unlock.
///
/// The register math is the same as `ra8_pfs_pmn`: 4 bytes per pin, 16 pins
/// per port. That helper is a static inline in a C header and so has no
/// symbol to call.
fn forcePinOutput(pin: u16) void {
    const port: u32 = pin >> 8;
    const index: u32 = pin & 0xFF;
    if (port > Pfs.port_max or index > Pfs.pin_max) return;

    const flat = (port * Pfs.pins_per_port) + index;
    const reg: *volatile u32 = @ptrFromInt(Pfs.base + (flat * @sizeOf(u32)));

    pwprUnlock();
    reg.* = (Psel.glcdc << Pfs.psel_shift) | Pfs.pmr_bit | Pfs.pdr_bit;
    pwprLock();
}

/// B0WI has to be cleared before PFSWE can be set. Both the non-secure and
/// the secure path are unlocked, so the write lands whichever world the CPU
/// is in; the one that does not own the port ignores it.
fn pwprUnlock() void {
    const pwpr: *volatile u8 = @ptrFromInt(Pfs.pmisc_base + Pfs.pwpr_off);
    const pwprs: *volatile u8 = @ptrFromInt(Pfs.pmisc_base + Pfs.pwprs_off);
    pwpr.* = 0;
    pwpr.* = 1 << Pfs.pfswe_bit;
    pwprs.* = 0;
    pwprs.* = 1 << Pfs.pfswe_bit;
}

fn pwprLock() void {
    const pwpr: *volatile u8 = @ptrFromInt(Pfs.pmisc_base + Pfs.pwpr_off);
    const pwprs: *volatile u8 = @ptrFromInt(Pfs.pmisc_base + Pfs.pwprs_off);
    pwpr.* = 0;
    pwpr.* = 1 << Pfs.b0wi_bit;
    pwprs.* = 0;
    pwprs.* = 1 << Pfs.b0wi_bit;
}

/// Route every GLCDC output of the chosen format. The non-GLCDC lines on the
/// connector are skipped, not refused: they belong to other subsystems.
pub fn init(fmt: u8) u32 {
    const table = glcdc_pins.tableFor(fmt) orelse return Err.invalid_arg;
    for (table) |entry| {
        if (!glcdc_pins.isOutput(entry.signal)) continue;
        const err = hal.ra8_pfs_route_peripheral(entry.pin, Psel.glcdc, "ra8_board.glcdc");
        if (err != Err.ok) return err;
        forcePinOutput(entry.pin);
    }
    return Err.ok;
}
