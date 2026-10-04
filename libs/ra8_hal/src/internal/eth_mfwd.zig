//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Ethernet MAC forwarding engine (inc/ra8_eth_mfwd.h, RA8FW-552): the
//! CTRL/STS/IE/ICLR block plus the FWPBFC / FWPBFCSDC forwarding fields.

pub const base: usize = 0x403C0000;

pub const off_ctrl: usize = 0x00;
pub const off_sts: usize = 0x04;
pub const off_ie: usize = 0x08;
pub const off_iclr: usize = 0x0C;

pub const off_fwpbfc0: usize = 0x4A00;
pub const off_fwpbfcsdc0: usize = 0x4A04;
pub const port_stride: usize = 0x10;

pub const pbdv_mask: u32 = 0x7F;
pub const pbcsd_mask: u32 = 0x7F;
/// Port[0], Port[1] and the host agent.
pub const port_count: usize = 3;
pub const max_port: u8 = 1;
pub const max_queue: u8 = 31;

pub const Error = error{InvalidArg};

/// The MFWD window; host tests point `base` at a fake block.
pub const Window = struct {
    base: usize = base,

    pub fn reg(w: Window, off: usize) *volatile u32 {
        return @ptrFromInt(w.base + off);
    }
};

/// Zero CTRL, STS, IE and ICLR (ra8_eth_mfwd_init after MSTP).
pub fn reset(w: Window) void {
    w.reg(off_ctrl).* = 0;
    w.reg(off_sts).* = 0;
    w.reg(off_ie).* = 0;
    w.reg(off_iclr).* = 0;
}

/// Zero CTRL and IE (ra8_eth_mfwd_deinit).
pub fn quiesce(w: Window) void {
    w.reg(off_ctrl).* = 0;
    w.reg(off_ie).* = 0;
}

pub fn status(w: Window) u32 {
    return w.reg(off_sts).*;
}

/// ICLR = mask, then STS &= ~mask.
pub fn clearStatus(w: Window, mask: u32) void {
    w.reg(off_iclr).* = mask;
    w.reg(off_sts).* = w.reg(off_sts).* & ~mask;
}

/// Read STS, clear it through ICLR and STS, return what was pending.
pub fn takeStatus(w: Window) u32 {
    const mask = w.reg(off_sts).*;
    w.reg(off_iclr).* = mask;
    w.reg(off_sts).* = 0;
    return mask;
}

/// Read-modify-write FWPBFCn.PBDV[6:0] for each of the three ports.
pub fn setForwardingMasks(w: Window, masks: *const [port_count]u8) void {
    for (masks, 0..) |m, i| {
        const r = w.reg(off_fwpbfc0 + i * port_stride);
        r.* = (r.* & ~pbdv_mask) | (@as(u32, m) & pbdv_mask);
    }
}

/// Read-modify-write FWPBFCSDCn.PBCSD[6:0] so port frames reach a queue.
pub fn routeQueue(w: Window, port: u8, queue: u8) Error!void {
    if (port > max_port or queue > max_queue) return error.InvalidArg;
    const r = w.reg(off_fwpbfcsdc0 + @as(usize, port) * port_stride);
    r.* = (r.* & ~pbcsd_mask) | (@as(u32, queue) & pbcsd_mask);
}
