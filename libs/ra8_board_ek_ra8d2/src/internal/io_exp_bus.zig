//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Getting the system I2C bus into a state where U15 can answer: clock a
//! wedged peripheral off SDA, enable the board's pull-ups, then hand
//! P512/P511 to the IIC1 mux in open-drain mode.
//!
//! U15 sits on RIIC channel 1, P512 (SCL1) and P511 (SDA1), not on the I3C
//! pair P400/P401. An earlier wiring talked to the wrong peripheral, which
//! is why every U15 access timed out.

const hal = @import("hal.zig");
const vocab = @import("vocab.zig");

const Err = vocab.Err;
const Io = vocab.Io;
const IoExpander = vocab.IoExpander;
const Pin = vocab.Pin;

pub const scl: u16 = Pin.pack(5, 12);
pub const sda: u16 = Pin.pack(5, 11);

/// UM Table 23: the SCL1/SDA1 pull-ups are routed through P109 and P311. In
/// I2C mode both must be driven high or the open-drain bus never reaches a
/// valid high level and nothing can ACK.
pub const pullup_a: u16 = Pin.pack(1, 9);
pub const pullup_b: u16 = Pin.pack(3, 11);

/// Roughly one SCL half-period. The recovery clocks do not need precise
/// timing, and this avoids depending on `ra8_time` being up this early.
fn settle() void {
    var i: u32 = 0;
    while (i < IoExpander.recover_spins) : (i += 1) {
        asm volatile ("nop");
    }
}

/// Bit-bang up to nine SCL clocks to free a wedged bus, then frame a STOP
/// and release both pins for the IIC mux. An aborted transfer can leave a
/// peripheral holding SDA low mid-byte, and every START fails until it is
/// clocked out.
pub fn recover() u32 {
    const scl_err = hal.ra8_gpio_output_init(scl, Io.level_high);
    if (scl_err != Err.ok) return scl_err;

    const sda_err = hal.ra8_gpio_input_init(sda, Io.pull_up);
    if (sda_err != Err.ok) {
        _ = hal.ra8_gpio_release(scl);
        return sda_err;
    }

    var pulse: u32 = 0;
    while (pulse < IoExpander.recover_pulses) : (pulse += 1) {
        var level: u32 = Io.level_low;
        if (hal.ra8_gpio_read(sda, &level) == Err.ok and level == Io.level_high) break;
        _ = hal.ra8_gpio_write(scl, Io.level_low);
        settle();
        _ = hal.ra8_gpio_write(scl, Io.level_high);
        settle();
    }

    // STOP is SDA rising while SCL is high: it ends any partial frame.
    _ = hal.ra8_gpio_release(sda);
    _ = hal.ra8_gpio_output_init(sda, Io.level_low);
    settle();
    _ = hal.ra8_gpio_write(scl, Io.level_high);
    _ = hal.ra8_gpio_write(sda, Io.level_high);
    settle();
    _ = hal.ra8_gpio_release(sda);
    _ = hal.ra8_gpio_release(scl);
    return Err.ok;
}

/// Drive both pull-up enables high. Runs before the route, so the bus is
/// pulled up the instant the IIC mux takes the pins.
pub fn enablePullups() u32 {
    const err = hal.ra8_gpio_output_init(pullup_a, Io.level_high);
    if (err != Err.ok) return err;
    return hal.ra8_gpio_output_init(pullup_b, Io.level_high);
}

/// Route both pins to IIC1 with N-channel open drain. The PFS route leaves
/// NCODR clear, so each pin takes a route and an open-drain write.
pub fn routePins() u32 {
    var err = hal.ra8_pfs_route_peripheral(scl, vocab.Psel.iic, "ra8_board.io_exp.scl1");
    if (err != Err.ok) return err;
    err = hal.ra8_mpc_set_open_drain(5, 12, true);
    if (err != Err.ok) return err;
    err = hal.ra8_pfs_route_peripheral(sda, vocab.Psel.iic, "ra8_board.io_exp.sda1");
    if (err != Err.ok) return err;
    return hal.ra8_mpc_set_open_drain(5, 11, true);
}
