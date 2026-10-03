//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! ICU external-IRQ and NMI control (RA8FW-538, ported from ra8_icu.c).
//! Offsets and bit fields match inc/ra8_icu_regs.h; its static inline
//! accessors stay for the C callers that still use them.

pub const base: usize = 0x4000_6000;
pub const off_irqcra0: usize = 0x0000;
pub const off_irqcrb0: usize = 0x0014;
pub const off_nmier: usize = 0x6100;
pub const off_nmiclr: usize = 0x6110;
pub const off_nmisr: usize = 0x6120;
pub const off_wupen0: usize = 0x61A0;
pub const off_wupen1: usize = 0x61A4;

pub const num_irqs: u8 = 32;
pub const irqcra_count: u8 = 16;
pub const nmiclr_all: u32 = 0xFFFF_FFFF;

pub const irqcr_mask_irqmd: u8 = 0x03;
pub const irqcr_bit_fclksel: u5 = 4;
pub const irqcr_mask_fclksel: u8 = 0x30;
pub const irqcr_mask_flten: u8 = 0x80;

/// `ra8_icu_irq_cfg_t` (inc/ra8_icu.h): sense and filter_div are uint8_t enums.
pub const Cfg = extern struct {
    sense: u8,
    filter_div: u8,
    filter_en: bool,
};

pub const Error = error{InvalidIrq};

/// The ICU register block. `hardware()` targets the real base; tests pass
/// the address of a fake buffer.
pub const Window = struct {
    base: usize,

    pub fn irqcr(self: Window, irq: u8) Error!*volatile u8 {
        if (irq >= num_irqs) return error.InvalidIrq;
        const off = if (irq < irqcra_count) off_irqcra0 + irq else off_irqcrb0 + (irq - irqcra_count);
        return @ptrFromInt(self.base + off);
    }

    pub fn reg32(self: Window, off: usize) *volatile u32 {
        return @ptrFromInt(self.base + off);
    }
};

pub fn hardware() Window {
    return .{ .base = base };
}

/// IRQCR value for `cfg`: IRQMD bits 1..0, FCLKSEL bits 5..4, FLTEN bit 7.
pub fn irqcrValue(cfg: Cfg) u8 {
    const fclk: u8 = @truncate(@as(u32, cfg.filter_div) << irqcr_bit_fclksel);
    var value = (cfg.sense & irqcr_mask_irqmd) | (fclk & irqcr_mask_fclksel);
    if (cfg.filter_en) value |= irqcr_mask_flten;
    return value;
}

/// Reset state: every IRQCR 0, NMIER 0, NMI status cleared, no wake-up source.
pub fn init(w: Window) void {
    var irq: u8 = 0;
    while (irq < num_irqs) : (irq += 1) (w.irqcr(irq) catch unreachable).* = 0;
    w.reg32(off_nmier).* = 0;
    w.reg32(off_nmiclr).* = nmiclr_all;
    w.reg32(off_wupen0).* = 0;
    w.reg32(off_wupen1).* = 0;
}

pub fn configureIrqPin(w: Window, irq: u8, cfg: Cfg) Error!void {
    (try w.irqcr(irq)).* = irqcrValue(cfg);
}

pub fn readIrqcr(w: Window, irq: u8) Error!u8 {
    return (try w.irqcr(irq)).*;
}

pub fn nmiEnable(w: Window, mask: u32) void {
    const nmier = w.reg32(off_nmier);
    nmier.* = nmier.* | mask;
}

pub fn nmiDisable(w: Window, mask: u32) void {
    const nmier = w.reg32(off_nmier);
    nmier.* = nmier.* & ~mask;
}

pub fn nmiClear(w: Window, mask: u32) void {
    w.reg32(off_nmiclr).* = mask;
}

pub fn nmiStatus(w: Window) u32 {
    return w.reg32(off_nmisr).*;
}
