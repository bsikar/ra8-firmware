//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The U15 PI4IOE5V6408 at I2C 0x43, which overrides the SW4 configuration
//! switches. UM Section 5.5.3.
//!
//! Output polarity is the FSP reference's: a HIGH output bit reads as SW4
//! OFF. Every public entry point here is the same bring-up with a different
//! SW4 layout latched.

const hal = @import("hal.zig");
const io_exp_bus = @import("io_exp_bus.zig");
const vocab = @import("vocab.zig");

const Err = vocab.Err;
const IoExpander = vocab.IoExpander;

/// Where the bring-up got to, readable by symbol from a debugger. It is the
/// only way to tell a bus that never came up from a U15 that would not ACK.
pub const Step = struct {
    pub const pre_pfs: u32 = 1;
    pub const pre_init: u32 = 2;
    pub const pre_write_out: u32 = 3;
    pub const pre_write_hiz: u32 = 4;
    pub const pre_write_dir: u32 = 5;
    pub const success: u32 = 6;
};

pub var probe: u32 = 0;

/// Each register write is a complete START..STOP transaction. Chaining them
/// with a repeated START leaves the bus held and the device's command parser
/// misaligned on the third back-to-back write.
fn writeReg(reg: u8, val: u8) u32 {
    const buf = [2]u8{ reg, val };
    return hal.ra8_i2c_write(IoExpander.iic_channel, IoExpander.addr_7b, &buf, buf.len, true);
}

/// Output latch, then Hi-Z clear, then direction: the FSP reference's order,
/// so a pin only becomes an output once the latch already drives the level
/// it should.
fn programU15(output_byte: u8, output_mask: u8) u32 {
    probe = Step.pre_write_out;
    var err = writeReg(IoExpander.reg_output, output_byte);
    if (err != Err.ok) return err;

    probe = Step.pre_write_hiz;
    err = writeReg(IoExpander.reg_hiz, IoExpander.hiz_none);
    if (err != Err.ok) return err;

    probe = Step.pre_write_dir;
    return writeReg(IoExpander.reg_iodir, output_mask);
}

/// Recover the bus, pull it up, route it, bring RIIC1 up at 100 kHz, then
/// latch the layout. `ra8_i2c_init` ungates the per-channel MSTP gate
/// itself, so there is no separate enable here.
pub fn applyMask(output_byte: u8, output_mask: u8) u32 {
    var err = io_exp_bus.recover();
    if (err != Err.ok) return err;

    err = io_exp_bus.enablePullups();
    if (err != Err.ok) return err;

    probe = Step.pre_pfs;
    err = io_exp_bus.routePins();
    if (err != Err.ok) return err;

    probe = Step.pre_init;
    const cfg = hal.I2cCfg{ .bus_hz = IoExpander.bus_hz, .pclkb_hz = IoExpander.pclkb_hz };
    err = hal.ra8_i2c_init(IoExpander.iic_channel, &cfg);
    if (err != Err.ok) return err;

    err = programU15(output_byte, output_mask);
    if (err != Err.ok) return err;

    probe = Step.success;
    return Err.ok;
}

/// Every SW4 channel at its mechanical-default OFF, which puts SW4-8 OFF and
/// so selects USB-HS device mode on J7.
pub fn setUsbhsDeviceMode() u32 {
    return applyMask(IoExpander.output_all_high, IoExpander.iodir_all_outputs);
}

/// The project layout with SW4-8 ON, so the board supplies VBUS on J7.
pub fn setUsbhsHostMode() u32 {
    return applyMask(IoExpander.output_usbhs_host, IoExpander.iodir_all_outputs);
}

/// This project's SW4 override: Pmod1 UART, Octo-SPI inactive, Arduino and
/// mikroBUS active, I2C rather than I3C, whatever the physical DIPs say.
pub fn applyProjectSw4Defaults() u32 {
    return applyMask(IoExpander.output_project_default, IoExpander.iodir_all_outputs);
}

pub fn applySw4(output_byte: u8) u32 {
    return applyMask(output_byte, IoExpander.iodir_all_outputs);
}

pub fn setOctospiActive() u32 {
    return applyMask(IoExpander.output_octospi_active, IoExpander.iodir_all_outputs);
}
