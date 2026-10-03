//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Independent watchdog refresh, status and event dispatch (RA8FW-541,
//! ported from ra8_iwdt.c). The counter auto-starts from OFS0 on RA8D2, so
//! there is nothing to configure at run time. Offsets and masks match
//! inc/ra8_iwdt_regs.h, whose static inline accessors stay for C callers.

pub const base: usize = 0x4020_2200;
pub const off_iwdtrr: usize = 0x00;
pub const off_iwdtsr: usize = 0x04;

pub const refresh_a: u8 = 0x00;
pub const refresh_b: u8 = 0xFF;

pub const sr_cnt_mask: u16 = 0x3FFF;
pub const status_underflow: u16 = 0x4000;
pub const status_refresh: u16 = 0x8000;
pub const status_all: u16 = status_underflow | status_refresh;

/// `ra8_iwdt_event_fn_t` (inc/ra8_iwdt.h).
pub const EventFn = *const fn (ctx: ?*anyopaque, status_mask: u16) callconv(.c) void;

pub const Handler = struct {
    func: ?EventFn = null,
    ctx: ?*anyopaque = null,
};

/// The IWDT register block. `hardware()` targets the real base; tests pass
/// the address of a fake buffer.
pub const Window = struct {
    base: usize,

    pub fn iwdtrr(self: Window) *volatile u8 {
        return @ptrFromInt(self.base + off_iwdtrr);
    }

    pub fn iwdtsr(self: Window) *volatile u16 {
        return @ptrFromInt(self.base + off_iwdtsr);
    }
};

pub fn hardware() Window {
    return .{ .base = base };
}

/// Reload the down-counter: 0x00 then 0xFF to IWDTRR, never one write.
pub fn refresh(w: Window) void {
    w.iwdtrr().* = refresh_a;
    w.iwdtrr().* = refresh_b;
}

/// The latched UNDFF / REFEF flags, CNTVAL masked out.
pub fn status(w: Window) u16 {
    return w.iwdtsr().* & status_all;
}

/// UNDFF and REFEF are write-0-to-clear; other bits are written back as read.
pub fn clearStatus(w: Window, mask: u16) void {
    const sr = w.iwdtsr();
    sr.* = sr.* & ~mask;
}

pub fn counter(w: Window) u16 {
    return w.iwdtsr().* & sr_cnt_mask;
}

/// ISR body: snapshot the flags, clear them, then hand the snapshot to the
/// handler when one is attached.
pub fn dispatch(w: Window, handler: Handler) void {
    const mask = status(w);
    clearStatus(w, mask);
    if (handler.func) |func| func(handler.ctx, mask);
}
