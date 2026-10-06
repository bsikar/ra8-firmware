//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! RTC IRQ enable, status, handler dispatch and deinit (RA8FW-852, was part of
//! ra8_rtc.c, now deleted). Exports live in src/rtc_events_abi.zig. RCR1 holds the
//! AIE/CIE/PIE bits 0..2 (HUM Ch 26.2.20 p 1231).

/// `k_ra8_rtc_irq_all`: alarm | carry | periodic.
pub const irq_all: u8 = 0x07;

/// `ra8_rtc_event_fn_t`.
pub const EventFn = *const fn (ctx: ?*anyopaque, status_mask: u8) callconv(.C) void;

pub const Handler = struct {
    func: ?EventFn = null,
    ctx: ?*anyopaque = null,
};

pub fn setIrqEnable(rcr1: *volatile u8, mask: u8) void {
    rcr1.* |= mask & irq_all;
}

pub fn status(rcr1: *volatile u8) u8 {
    return rcr1.* & irq_all;
}

pub fn clearStatus(rcr1: *volatile u8, mask: u8) void {
    rcr1.* &= ~(mask & irq_all);
}

/// Mask every IRQ (RCR1 = 0), stop the counter (RCR2 = 0), drop the handler.
pub fn deinit(rcr1: *volatile u8, rcr2: *volatile u8, h: *Handler) void {
    rcr1.* = 0;
    rcr2.* = 0;
    h.* = .{};
}

/// Hand the enabled-source mask to the attached handler, if any.
pub fn dispatch(rcr1: *volatile u8, h: *const Handler) void {
    const mask = status(rcr1);
    if (h.func) |f| f(h.ctx, mask);
}
