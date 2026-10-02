//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The one word pair both cores of threadx_cpu1 agree on, in shared SRAM at
//! the address dualcore_mailbox already uses (SRAM1 upper half, unused with
//! TrustZone off). CPU1's ThreadX thread writes it; the M85 only reads it
//! after clearing it before the release.

pub const address: usize = 0x2210_0000;
/// "TX33": CPU1's thread is running.
pub const signature: u32 = 0x5458_3333;
/// The tick count the M85 waits for before it prints PASS.
pub const pass_ticks: u32 = 10;

pub const Block = extern struct {
    signature: u32,
    ticks: u32,
};

pub fn block() *volatile Block {
    return @ptrFromInt(address);
}
