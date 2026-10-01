//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! USB-HS bring-up: the PHY clock and MSTP gate both modes share, the PD07
//! role strap, and the two public entry points.

const hal = @import("hal.zig");
const io_expander = @import("io_expander.zig");
const vocab = @import("vocab.zig");

const Err = vocab.Err;
const Io = vocab.Io;
const Pin = vocab.Pin;
const Usb = vocab.Usb;

/// Bring-up progress, readable by symbol from a debugger. These were added
/// to bisect a HardFault during USBHS bring-up and are kept because they are
/// the only on-target signal of which sub-call was in flight.
pub const Step = struct {
    pub const pre_pll_enable: u32 = 1;
    pub const pre_mstp_init: u32 = 2;
    pub const pre_mstp_enable: u32 = 3;
    pub const pre_usb_dev_init: u32 = 4;
    pub const post_usb_dev_init: u32 = 5;
};

pub const RoleStep = struct {
    pub const pre_init: u32 = 1;
    pub const post_init: u32 = 2;
    pub const success: u32 = 3;
};

/// Exported under their C names by the ABI layer, because a bench session
/// resolves them by symbol.
pub var probe: u32 = 0;
pub var role_probe: u32 = 0;
pub var role_err: u32 = 0;

/// PD07, port 13 pin 7.
pub const role_pin: u16 = Pin.pack(Usb.role_pin_port, Usb.role_pin_index);

/// Arm the PHY's 12 MHz reference, then ungate MSTPCRB.MSTPB12 so the
/// controller's SYSCFG block is reachable.
///
/// `ra8_mstp_init` deliberately is NOT called here. It gates every module,
/// including ones already running, and the chip faults reaching them. It
/// runs exactly once per boot, from the boot path, before any peripheral
/// comes up; `ra8_mstp_enable` below uses that ref-counted state.
pub fn clockAndMstp() u32 {
    probe = Step.pre_pll_enable;
    const err = hal.ra8_cgc_usbhs_pll_enable();
    if (err != Err.ok) return err;
    probe = Step.pre_mstp_enable;
    return hal.ra8_mstp_enable(Usb.mstp_usbhs);
}

/// Drive PD07 low to strap J7 to Device. UM 6.2 p 34: low is Device, high is
/// Host. This is a plain MCU GPIO; U15 only matters when firmware needs to
/// override the SW4-8 strap from somewhere else.
pub fn roleSelectDevice() u32 {
    role_probe = RoleStep.pre_init;
    const err = hal.ra8_gpio_output_init(role_pin, Io.level_low);
    role_err = err;
    role_probe = RoleStep.post_init;
    if (err == Err.ok) role_probe = RoleStep.success;
    return err;
}

/// PD07 strap, then a best-effort U15 override, then clock, gate and the
/// generic device-mode entry.
///
/// The U15 write is deliberately non-fatal: it can NACK when SW4-5 is OFF
/// and the shared bus is routed elsewhere, and PD07 has already strapped the
/// role by then.
pub fn deviceInit() u32 {
    const pd07_err = roleSelectDevice();
    if (pd07_err != Err.ok) return pd07_err;

    _ = io_expander.setUsbhsDeviceMode();

    const err = clockAndMstp();
    if (err != Err.ok) return err;

    probe = Step.pre_usb_dev_init;
    const rc = hal.ra8_usb_device_init(Usb.speed_hs);
    probe = Step.post_usb_dev_init;
    return rc;
}

/// Symmetric to `deviceInit`, minus the strap. UM Table 28 lists
/// USBHS_VBUSEN / USBHS_OVRCUR as PHY-controller pins that are not routed to
/// RA8D2 port pins, so VBUS sourcing is the on-board USB-PD controller's job.
pub fn hostInit() u32 {
    const err = clockAndMstp();
    if (err != Err.ok) return err;
    return hal.ra8_usb_host_init(Usb.speed_hs);
}
