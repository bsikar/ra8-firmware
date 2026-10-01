//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Board-coordinate pin tables: which `ra8_port_pin_t` each LED and switch sits
//! on, and which ICU IRQ channel each switch latches.
//!
//! The pins are PROVISIONAL, mirrored from the pin-compatible EK-RA8D2 layout
//! because no RA8P1 board is defined yet (no committed EK-RA8P1 User's Manual,
//! no `ra8p1_kicad/` PCB). Every pin is a valid GPIO / SCI alternate on the
//! RA8P1 (chip HUM R01UH1064EJ Ch 20 "I/O Ports").
//! TODO(EK-RA8P1 UM / ra8p1_kicad): re-derive from the schematic.

/// Re-exported so a test rooted at this file reaches the shared vocabulary
/// without pulling `vocab.zig` into a second module of the same binary.
pub const vocab = @import("vocab.zig");

const Pin = vocab.Pin;

/// `ra8_board_led_id_t` ordinals.
pub const Led = struct {
    pub const led1: u8 = 0;
    pub const led2: u8 = 1;
    pub const led3: u8 = 2;
    pub const count: u8 = 3;
};

/// `ra8_board_sw_id_t` ordinals.
pub const Sw = struct {
    pub const sw1: u8 = 0;
    pub const sw2: u8 = 1;
    pub const count: u8 = 2;
};

/// ICU IRQ channels behind the switches (`ra8_board_sw_irq_t`).
pub const SwIrq = struct {
    pub const sw1: u8 = 13;
    pub const sw2: u8 = 12;
};

/// ELC event ids for those IRQ channels (libs/ra8_hal/inc/ra8_elc_regs.h).
pub const SwEvent = struct {
    pub const irq12: u16 = 0x00D;
    pub const irq13: u16 = 0x00E;
};

/// LED id -> pin. P600, P303, PA07 (provisional).
const led_pins = [Led.count]u16{
    Pin.pack(6, 0),
    Pin.pack(3, 3),
    Pin.pack(10, 7),
};

/// Switch id -> pin. P009, P008 (provisional).
const sw_pins = [Sw.count]u16{
    Pin.pack(0, 9),
    Pin.pack(0, 8),
};

/// Switch id -> ICU IRQ channel.
const sw_irq_nums = [Sw.count]u8{ SwIrq.sw1, SwIrq.sw2 };

pub fn ledPin(led: u8) ?u16 {
    if (led >= Led.count) return null;
    return led_pins[led];
}

pub fn swPin(sw: u8) ?u16 {
    if (sw >= Sw.count) return null;
    return sw_pins[sw];
}

pub fn swIrqNum(sw: u8) ?u8 {
    if (sw >= Sw.count) return null;
    return sw_irq_nums[sw];
}

/// ELC event the switch's IRQ channel raises. SW1 -> IRQ13, SW2 -> IRQ12.
pub fn swEvent(sw: u8) ?u16 {
    if (sw >= Sw.count) return null;
    return if (sw == Sw.sw1) SwEvent.irq13 else SwEvent.irq12;
}

/// A button is active-low: a low level means held.
pub fn pressed(level: u8) bool {
    return level == vocab.Level.low;
}
