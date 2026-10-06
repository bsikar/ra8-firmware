//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! What both cores of cpu1_routed_irq agree on (RA8FW-809): the routed event,
//! the registers that route it, and the block in shared SRAM (SRAM1 upper
//! half, the address threadx_cpu1 uses) where CPU1 reports.

/// GPT0 counter overflow (GPT0_COUNTER_OVERFLOW) in the RA8D2 event table.
pub const event: u32 = 0xC1;
/// CPU0's COMMON_ICU INTSELRn: one bit per event, set to hand it to CPU1.
pub const intselr_base: usize = 0x4000_6040;
/// CPU1's ICU IELSR0: links an event to CPU1's NVIC line 0.
pub const cpu1_ielsr0: usize = 0x4000_C300;
/// IELSR.IR: the event latched; write zero to clear.
pub const ielsr_ir: u32 = 1 << 16;

pub const address: usize = 0x2210_0000;

pub const Block = extern struct {
    /// CPU1 has linked the event and enabled its NVIC line.
    armed: u32,
    /// CPU1's handler ran.
    irq_count: u32,
};

pub fn block() *volatile Block {
    return @ptrFromInt(address);
}

/// The INTSELRn word that carries `event`.
pub fn intselr() *volatile u32 {
    return @ptrFromInt(intselr_base + 4 * (event / 32));
}

/// `event`'s bit in that word.
pub fn intselrBit() u32 {
    return @as(u32, 1) << @intCast(event % 32);
}
