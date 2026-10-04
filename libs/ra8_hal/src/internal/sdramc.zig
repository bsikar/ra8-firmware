//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! SDRAM controller bring-up and refresh control for the IS42S32160F on
//! EK-RA8D2 (RA8FW-608), behind ra8_sdramc.h. `hw` provides
//! read8/write8/write16/write32(off) on the SDRAMC block, prcr(u16),
//! sdckocr(u8), route(pin) u16, drive(pin) u16, zeroBss() u16,
//! err(msg) and info(msg).

pub const ok: u16 = 0;
pub const invalid_state: u16 = 0x104;
pub const hw_timeout: u16 = 0x203;
pub const null_ptr: u16 = 0x504;

pub const base: usize = 0x4000_3C00;
pub const sdckocr_addr: usize = 0x4001_E053;
pub const prcr_addr: usize = 0x4001_E3FA;

pub const off_sdccr: usize = 0x00;
pub const off_sdcmod: usize = 0x01;
pub const off_sdamod: usize = 0x02;
pub const off_sdself: usize = 0x10;
pub const off_sdrfcr: usize = 0x14;
pub const off_sdrfen: usize = 0x16;
pub const off_sdicr: usize = 0x20;
pub const off_sdir: usize = 0x24;
pub const off_sdadr: usize = 0x40;
pub const off_sdtr: usize = 0x44;
pub const off_sdmod: usize = 0x48;
pub const off_sdsr: usize = 0x50;

pub const sdccr_cfg_32bit: u8 = 0x10;
pub const sdccr_enable: u8 = 0x11;
pub const sdamod_be: u8 = 0x01;
pub const sdadr_mxc_9col: u8 = 0x01;
pub const sdir_init: u16 = 0x0088;
pub const sdmod_lmr: u16 = 0x0230;
pub const sdrfcr_init: u16 = 0xB383;
pub const sdtr_init: u32 = 0x0005_3703;
pub const kick: u8 = 0x01;

pub const sdsr_mrsst: u8 = 0x01;
pub const sdsr_inist: u8 = 0x08;
pub const sdsr_srfst: u8 = 0x10;
pub const sdsr_all: u8 = 0x19;
pub const sdself_sfen: u8 = 0x01;

pub const prcr_unlock_cgc: u16 = 0xA501;
pub const prcr_lock_all: u16 = 0xA500;
pub const spin_max: u32 = 1_000_000;

fn pin(port: u16, n: u16) u16 {
    return (port << 8) | n;
}

/// The 57 external-bus pins, in the order the C routed them (A0 first).
pub const bus_pins = [_]u16{
    pin(10, 3),  pin(10, 2),  pin(10, 1),  pin(10, 0),  pin(5, 3),   pin(5, 4),
    pin(5, 5),   pin(5, 6),   pin(5, 7),   pin(5, 8),   pin(5, 9),   pin(5, 10),
    pin(6, 8),   pin(13, 0),  pin(12, 15), pin(3, 2),   pin(3, 1),   pin(3, 0),
    pin(1, 12),  pin(1, 13),  pin(1, 14),  pin(1, 15),  pin(6, 9),   pin(10, 11),
    pin(10, 12), pin(10, 13), pin(10, 14), pin(6, 10),  pin(6, 11),  pin(6, 12),
    pin(6, 13),  pin(12, 14), pin(12, 13), pin(12, 12), pin(12, 11), pin(12, 10),
    pin(12, 9),  pin(12, 8),  pin(12, 7),  pin(12, 6),  pin(12, 5),  pin(12, 4),
    pin(12, 3),  pin(12, 2),  pin(12, 1),  pin(12, 0),  pin(6, 7),   pin(10, 6),
    pin(10, 15), pin(6, 14),  pin(10, 5),  pin(6, 15),  pin(10, 4),  pin(10, 8),
    pin(10, 9),  pin(10, 10), pin(8, 13),
};

/// Route every bus pin to the BUS function at high-speed-high drive.
pub fn routePins(hw: anytype) u16 {
    for (bus_pins) |p| {
        const route_err = hw.route(p);
        if (route_err != ok) return route_err;
        const drive_err = hw.drive(p);
        if (drive_err != ok) return drive_err;
    }
    return ok;
}

/// Bounded poll until every SDSR bit in `mask` reads clear.
pub fn wait(hw: anytype, mask: u8) u16 {
    var spin: u32 = 0;
    while (spin < spin_max) : (spin += 1) {
        if (hw.read8(off_sdsr) & mask == 0) return ok;
    }
    hw.err("sdramc: SDSR status bits never cleared");
    return hw_timeout;
}

/// `ra8_sdramc_init`: HUM Ch 15.6.10 bring-up, then zero `.sdram_data`.
pub fn init(hw: anytype) u16 {
    const pin_err = routePins(hw);
    if (pin_err != ok) {
        hw.err("sdramc: bus-pin routing failed");
        return pin_err;
    }
    var e = wait(hw, sdsr_all);
    if (e != ok) return e;
    hw.write16(off_sdir, sdir_init);
    hw.write8(off_sdccr, sdccr_cfg_32bit);
    hw.prcr(prcr_unlock_cgc);
    hw.sdckocr(kick);
    hw.prcr(prcr_lock_all);
    hw.write8(off_sdicr, kick);
    e = wait(hw, sdsr_inist);
    if (e != ok) return e;
    hw.write8(off_sdamod, sdamod_be);
    hw.write8(off_sdcmod, 0);
    e = wait(hw, sdsr_all);
    if (e != ok) return e;
    hw.write16(off_sdmod, sdmod_lmr);
    e = wait(hw, sdsr_mrsst);
    if (e != ok) return e;
    hw.write32(off_sdtr, sdtr_init);
    hw.write8(off_sdadr, sdadr_mxc_9col);
    hw.write16(off_sdrfcr, sdrfcr_init);
    hw.write8(off_sdrfen, kick);
    hw.write8(off_sdccr, sdccr_enable);
    const fill_err = hw.zeroBss();
    if (fill_err != ok) {
        hw.err("sdramc: .sdram_data zero-fill failed");
        return fill_err;
    }
    hw.info("sdramc_init (64 MiB @ 0x68000000)");
    return ok;
}

/// `ra8_sdramc_deinit`: stop auto-refresh, then disable the controller.
pub fn deinit(hw: anytype) u16 {
    hw.write8(off_sdrfen, 0);
    hw.write8(off_sdccr, 0);
    return ok;
}

pub fn setRefreshInterval(hw: anytype, sdrfcr: u16) u16 {
    hw.write16(off_sdrfcr, sdrfcr);
    return ok;
}

pub fn getStatus(hw: anytype, out_opt: ?*u8) u16 {
    const out = out_opt orelse {
        hw.err("out_enabled must not be nullptr");
        return null_ptr;
    };
    out.* = hw.read8(off_sdrfen);
    return ok;
}

pub fn enterStop(hw: anytype) u16 {
    hw.write8(off_sdrfen, 0);
    return ok;
}

pub fn exitStop(hw: anytype) u16 {
    hw.write8(off_sdrfen, kick);
    return ok;
}

/// `ra8_sdramc_enter_self_refresh`: SDSR idle and SFEN clear first.
pub fn enterSelfRefresh(hw: anytype) u16 {
    if (hw.read8(off_sdsr) & sdsr_all != 0) {
        hw.err("sdramc: SDSR busy on self-refresh entry");
        return invalid_state;
    }
    if (hw.read8(off_sdself) & sdself_sfen != 0) {
        hw.err("sdramc: already in self-refresh");
        return invalid_state;
    }
    hw.write8(off_sdrfen, 0);
    hw.write8(off_sdself, sdself_sfen);
    const e = wait(hw, sdsr_srfst);
    if (e != ok) return e;
    hw.info("sdramc: entered self-refresh");
    return ok;
}

/// `ra8_sdramc_exit_self_refresh`: SFEN set and SRFST clear first.
pub fn exitSelfRefresh(hw: anytype) u16 {
    if (hw.read8(off_sdself) & sdself_sfen == 0) {
        hw.err("sdramc: not in self-refresh on exit");
        return invalid_state;
    }
    if (hw.read8(off_sdsr) & sdsr_srfst != 0) {
        hw.err("sdramc: SDSR SRFST busy on self-refresh exit");
        return invalid_state;
    }
    hw.write8(off_sdself, 0);
    const e = wait(hw, sdsr_srfst);
    if (e != ok) return e;
    hw.write8(off_sdrfen, kick);
    hw.info("sdramc: exited self-refresh");
    return ok;
}
