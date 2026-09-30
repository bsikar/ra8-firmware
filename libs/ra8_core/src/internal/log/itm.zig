//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The ITM trace transport: the debugger-backed sink the log backend uses on
//! target when no byte sink is installed.
//!
//! Everything here is target-only. A host build has no ITM and no MMIO to
//! reach, so `ready` reports unavailable and `put` drops the byte, which is
//! exactly what the C did behind `RA8_OFF_TARGET`.

const builtin = @import("builtin");

/// True on a freestanding image, false in a host test binary. The C spelled
/// this `RA8_OFF_TARGET`, inverted.
pub const on_target = builtin.target.os.tag == .freestanding;

/// ITM register addresses, from the Armv8-M architecture reference manual.
pub const window = struct {
    /// Stimulus port 0.
    pub const stim0: usize = 0xE000_0000;
    /// Trace Control Register; bit 0 is ITMENA.
    pub const tcr: usize = 0xE000_0E80;
    /// Trace Enable Register; bit 0 enables stimulus port 0.
    pub const tenr: usize = 0xE000_0E00;
};

/// How many times `put` checks for stimulus-port space before dropping the
/// byte. A disconnected debugger must never stall the firmware.
pub const poll_limit: u32 = 1000;

const enable_bit: u32 = 1;

fn reg(comptime address: usize) *volatile u32 {
    return @ptrFromInt(address);
}

extern fn ra8_scb_trace_enabled() bool;

/// Whether the ITM block can be written right now.
///
/// The DEMCR.TRCENA pre-check is load-bearing, not hygiene: reading any ITM
/// register with the block powered down bus-faults, and a fault handler that
/// logs would fault a second time and escalate to LOCKUP, hiding the original
/// PC. That was a real USB bring-up failure.
pub fn ready() bool {
    if (!on_target) return false;

    if (!ra8_scb_trace_enabled()) return false;

    // Never poke ITM from an exception context. Dropping a log line beats a
    // second fault masking the first one's PC.
    if (inException()) return false;

    if (reg(window.tcr).* & enable_bit == 0) return false;
    if (reg(window.tenr).* & enable_bit == 0) return false;
    return reg(window.stim0).* != 0;
}

/// IPSR is non-zero inside any exception handler.
fn inException() bool {
    const ipsr = asm volatile ("mrs %[out], ipsr"
        : [out] "=r" (-> u32),
    );
    return ipsr != 0;
}

/// Write one byte to stimulus port 0, dropping it if the port stays full.
pub fn put(byte: u8) void {
    if (!on_target) return;

    var spins: u32 = 0;
    while (spins < poll_limit) : (spins += 1) {
        if (reg(window.stim0).* != 0) {
            @as(*volatile u8, @ptrFromInt(window.stim0)).* = byte;
            return;
        }
    }
}
