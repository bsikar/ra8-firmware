//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Pin routing for the two USB connectors. The one board fact worth keeping in
//! a single place: VBUSEN must stay a GPIO. Routing it to the USBFS peripheral
//! function makes the controller drive it as host VBUSEN and device
//! enumeration never completes, so the role is strapped by driving P5_00.

const hal = @import("hal.zig");
const usbhs = @import("usbhs.zig");
const vocab = @import("vocab.zig");

pub const Err = vocab.Err;

/// `ra8_board_usb_port_t`.
pub const Port = struct {
    pub const fs: u32 = 0;
    pub const hs: u32 = 1;
};

/// `ra8_board_usb_role_t`.
pub const Role = struct {
    pub const device: u32 = 0;
    pub const host: u32 = 1;
};

/// The four full-speed pins on J11, packed port/pin.
pub const FsPin = struct {
    pub const dp: u16 = vocab.Pin.pack(8, 14);
    pub const dm: u16 = vocab.Pin.pack(8, 15);
    pub const vbus: u16 = vocab.Pin.pack(4, 7);
    pub const vbusen: u16 = vocab.Pin.pack(5, 0);
};

/// Route D+, D- and VBUS sense to the USBFS peripheral function.
///
/// VBUSEN is deliberately absent: see `strapRole`.
fn routeFsPins() u32 {
    const pins = [_]struct { pin: u16, owner: [*:0]const u8 }{
        .{ .pin = FsPin.vbus, .owner = "board.usbfs.vbus" },
        .{ .pin = FsPin.dp, .owner = "board.usbfs.dp" },
        .{ .pin = FsPin.dm, .owner = "board.usbfs.dm" },
    };
    for (pins) |entry| {
        const err = hal.ra8_pfs_route_peripheral(entry.pin, vocab.Psel.usb_fs, entry.owner);
        if (err != Err.ok) return err;
    }
    return Err.ok;
}

/// Drive P5_00 as a GPIO: low for device, high to supply 5 V as host.
fn strapRole(role: u32) u32 {
    const level = if (role == Role.host) vocab.Level.high else vocab.Level.low;
    return hal.ra8_gpio_output_init(FsPin.vbusen, level);
}

/// Bring one USB connector up in the named role.
pub fn init(port: u32, role: u32) u32 {
    if ((role != Role.device) and (role != Role.host)) return Err.invalid_arg;

    if (port == Port.fs) {
        const err = routeFsPins();
        if (err != Err.ok) return err;
        return strapRole(role);
    }
    if (port == Port.hs) {
        return if (role == Role.host)
            usbhs.hostInit()
        else
            usbhs.deviceInit();
    }
    return Err.invalid_arg;
}
