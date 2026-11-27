//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The off-board Ethernet PHY on the EK-RA8D2: a MaxLinear GPY111 (PEF7071).
//! Two unrelated-looking jobs, both of them "get this chip into a usable
//! state": the hardware reset line, which is GPIO and must run before any
//! clock exists, and the chip-init over MDIO, which needs the RMAC up first.

const busy_wait = @import("busy_wait.zig");
const hal = @import("hal.zig");
const pins = @import("eth_pins.zig");
const vocab = @import("vocab.zig");

pub const Err = vocab.Err;

/// Reset-pulse lengths, in nop iterations. Both are minimums from the GPY111
/// datasheet (Section 4.1 "Power-on and Reset Sequence"), held before any timer
/// this board owns is running. At 1 GHz Cortex-M85 clock frequency (1-2 cycles
/// per loop iteration), 25M / 80M iterations provide deterministic >= 25 ms /
/// >= 80 ms timings across repeated warm-reset cycles.
pub const Reset = struct {
    /// >= 15 ms with RSTN low (datasheet specifies >= 15 ms; 25,000,000 nop
    /// iterations guarantees >= 25 ms on Cortex-M85).
    pub const low_iters: u32 = 25_000_000;
    /// >= 60 ms after RSTN releases, before the first MDIO access (datasheet
    /// specifies >= 60 ms; 80,000,000 nop iterations guarantees >= 80 ms).
    pub const post_iters: u32 = 80_000_000;
    /// Cap on polling BMCR.RESET for its self-clear.
    pub const soft_spin: u32 = 4096;
    /// Cap on polling BMSR for auto-negotiation complete and link up.
    /// At 1 MHz MDC, 25,000 reads provide a ~2.0 s bounded wait.
    pub const an_spin: u32 = 25_000;
};

/// Clause-22 register numbers and the bit fields this board writes.
pub const Reg = struct {
    pub const bmcr: u8 = 0;
    pub const bmsr: u8 = 1;
    pub const anar: u8 = 4;
    pub const gbcr: u8 = 9;
    /// GPY111 vendor register carrying the RGMII skew fields.
    pub const miictrl: u8 = 0x17;
};

pub const Bits = struct {
    /// BMCR.RESET, self-clearing.
    pub const bmcr_reset: u16 = 0x8000;
    pub const bmcr_an_enable: u16 = 0x1000;
    pub const bmcr_an_restart: u16 = 0x0200;
    /// BMSR bits (IEEE 802.3 Clause 22 / GPY111 register 1).
    pub const bmsr_link_up: u16 = 0x0004;
    pub const bmsr_an_complete: u16 = 0x0020;
    /// MIICTRL RXSKEW field [14:12].
    pub const rxskew_mask: u16 = 0x7000;
    /// RXSKEW = 0b010, i.e. 1.0 ns.
    pub const rxskew_1p0ns: u16 = 0x2000;
    /// Advertise 100F / 100H / 10F / 10H.
    pub const anar_value: u16 = 0x01E1;
    /// 1000BASE-T deliberately not advertised, so the link settles at 100 Mbit
    /// full duplex and the RMAC's MII-mode internal interface stays valid.
    pub const gbcr_value: u16 = 0x0000;
};

/// MDIO address of the on-board PHY, strapped to 0.
pub const addr: u8 = 0;

/// Pulse RSTN low then high, with the datasheet dwells either side.
///
/// Runs before the pin routing, because a PHY still in reset must not see
/// RGMII traffic, and before any clock, which is why the dwells are nops.
pub fn hardwareReset() u32 {
    const init_err = hal.ra8_gpio_output_init(pins.rstn, vocab.Level.low);
    if (init_err != Err.ok) return init_err;

    busy_wait.spin(Reset.low_iters);

    // Off target this cannot fail: output_init already accepted this fixed
    // mapped P708 pin. Forwarded rather than ignored all the same.
    const write_err = hal.ra8_gpio_write(pins.rstn, vocab.Level.high);
    if (write_err != Err.ok) return write_err;

    busy_wait.spin(Reset.post_iters);
    return Err.ok;
}

fn read(reg: u8, out: *u16) u32 {
    return hal.ra8_rmac_mdio_c22_read(vocab.Eth.rmac_port, addr, reg, out);
}

fn write(reg: u8, value: u16) u32 {
    return hal.ra8_rmac_mdio_c22_write(vocab.Eth.rmac_port, addr, reg, value);
}

/// Assert BMCR.RESET and wait for the PHY to clear it.
fn softReset() u32 {
    const err = write(Reg.bmcr, Bits.bmcr_reset);
    if (err != Err.ok) return err;

    var spins: u32 = 0;
    while (spins < Reset.soft_spin) : (spins += 1) {
        var bmcr: u16 = 0;
        const read_err = read(Reg.bmcr, &bmcr);
        if (read_err != Err.ok) return read_err;
        if ((bmcr & Bits.bmcr_reset) == 0) return Err.ok;
    }
    return Err.hw_timeout;
}

/// Set the receive clock skew to 1.0 ns, leaving the rest of MIICTRL alone.
fn setRgmiiSkew() u32 {
    var miictrl: u16 = 0;
    const read_err = read(Reg.miictrl, &miictrl);
    if (read_err != Err.ok) return read_err;

    miictrl = (miictrl & ~Bits.rxskew_mask) | Bits.rxskew_1p0ns;
    return write(Reg.miictrl, miictrl);
}

/// Advertise 10/100 only, then restart auto-negotiation.
fn startAutoneg() u32 {
    const anar_err = write(Reg.anar, Bits.anar_value);
    if (anar_err != Err.ok) return anar_err;

    const gbcr_err = write(Reg.gbcr, Bits.gbcr_value);
    if (gbcr_err != Err.ok) return gbcr_err;

    return write(Reg.bmcr, Bits.bmcr_an_enable | Bits.bmcr_an_restart);
}

/// Poll BMSR for auto-negotiation complete and link up (HUM Ch 33.4.1.2).
/// Bounded poll: returns ok when link and autoneg are complete, or ok on timeout
/// if no link partner is connected so headless and bare-metal boots do not hang.
pub fn waitForAutoneg() u32 {
    var spins: u32 = 0;
    while (spins < Reset.an_spin) : (spins += 1) {
        var bmsr: u16 = 0;
        const read_err = read(Reg.bmsr, &bmsr);
        if (read_err != Err.ok) return read_err;
        if ((bmsr & (Bits.bmsr_an_complete | Bits.bmsr_link_up)) == (Bits.bmsr_an_complete | Bits.bmsr_link_up)) {
            return Err.ok;
        }
    }
    return Err.ok;
}

/// Soft-reset the PHY, fix the RGMII receive skew, restart negotiating, and
/// wait for auto-negotiation to complete. Needs the RMAC up, since every step
/// rides MDIO.
pub fn chipInit() u32 {
    const reset_err = softReset();
    if (reset_err != Err.ok) return reset_err;

    const skew_err = setRgmiiSkew();
    if (skew_err != Err.ok) return skew_err;

    const an_err = startAutoneg();
    if (an_err != Err.ok) return an_err;

    return waitForAutoneg();
}
