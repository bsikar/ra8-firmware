//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The two user buttons, active-low, both deep-sleep wake capable.
//! UM Table 25 p 32. SW3 is wired to chip RESET_L and is not readable.

const hal = @import("hal.zig");
const vocab = @import("vocab.zig");

const Err = vocab.Err;
const Io = vocab.Io;
const Level = vocab.Level;
const Pin = vocab.Pin;
const Sw = vocab.Sw;

/// SW1 -> P009 / IRQ13-DS, SW2 -> P008 / IRQ12-DS.
pub const pins = [_]u16{ Pin.pack(0, 9), Pin.pack(0, 8) };

/// ICU channel per switch, in the same order as `pins`.
pub const irq_nums = [_]u8{ Sw.sw1_irq, Sw.sw2_irq };

/// ELC event per switch: IRQ13-DS is 0x00E, IRQ12-DS is 0x00D.
pub const events = [_]u16{ Sw.event_irq13, Sw.event_irq12 };

pub fn pinOf(sw: u8) ?u16 {
    if (sw >= pins.len) return null;
    return pins[sw];
}

pub fn readPin(sw: u8, out_pin: *u16) u32 {
    const pin = pinOf(sw) orelse return Err.invalid_arg;
    out_pin.* = pin;
    return Err.ok;
}

pub fn init(sw: u8) u32 {
    const pin = pinOf(sw) orelse return Err.invalid_arg;
    return hal.ra8_gpio_input_init(pin, Io.pull_up);
}

/// Buttons are active-low, so a low level reads as pressed.
pub fn read(sw: u8, out_pressed: *u8) u32 {
    const pin = pinOf(sw) orelse return Err.invalid_arg;
    var level: u32 = Level.high;
    const err = hal.ra8_gpio_read(pin, &level);
    if (err != Err.ok) return err;
    out_pressed.* = if (level == Level.low) Sw.pressed else Sw.released;
    return Err.ok;
}

/// Falling-edge detect with the digital filter sampling at PCLKB, then the
/// ELC event routed to an IELSR slot and its NVIC line enabled.
pub fn attachIrq(sw: u8, cb: ?*const fn (?*anyopaque) callconv(.c) void, ctx: ?*anyopaque) u32 {
    const handler = cb orelse return Err.invalid_arg;
    if (sw >= pins.len) return Err.invalid_arg;

    const cfg = hal.IcuIrqCfg{
        .sense = Sw.irqmd_falling,
        .filter_div = Sw.fclksel_pclkb,
        .filter_en = true,
    };
    const err = hal.ra8_icu_configure_irq_pin(irq_nums[sw], &cfg);
    if (err != Err.ok) return err;

    return hal.ra8_isr_register(events[sw], handler, ctx, Sw.isr_prio_default, null);
}
