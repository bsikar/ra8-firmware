//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! SPI_B target (peripheral) mode, one byte per exchange (RA8FW-596). Pure:
//! `regs` reads and writes a channel's 32-bit registers by offset, `c`
//! reaches module stop and logging. Offsets follow r_spi_regs_t.

pub const ok: u16 = 0;
pub const err_invalid_arg: u16 = 0x103;
pub const err_hw_timeout: u16 = 0x203;
pub const err_null_ptr: u16 = 0x504;

pub const base0: usize = 0x4035C000;
pub const stride: usize = 0x100;
pub const channel_count: u8 = 2;
pub const poll_limit: u32 = 200000;

pub const off_spdr: usize = 0x00;
pub const off_spdecr: usize = 0x04;
pub const off_spcr: usize = 0x08;
pub const off_spcr2: usize = 0x0C;
pub const off_spcr3: usize = 0x10;
pub const off_spcmd0: usize = 0x14;
pub const off_spdcr: usize = 0x40;
pub const off_spdcr2: usize = 0x44;
pub const off_spsr: usize = 0x50;
pub const off_spsrc: usize = 0x68;
pub const off_spfcr: usize = 0x6C;

pub const spsr_sptef: u32 = 0x2000_0000;
pub const spsr_sprf: u32 = 0x8000_0000;
pub const spsrc_all: u32 = 0xFD80_0000;
pub const spcr_target: u32 = 0x4001; // SPE | MODFEN
const spfcr_spfrst: u32 = 0x1;

/// MSTPB19 SPI0, MSTPB18 SPI1.
pub fn mstpId(channel: u8) u16 {
    return (1 << 8) | @as(u16, 19 - channel);
}

/// `ra8_spi_cfg_t`.
pub const Cfg = extern struct {
    baud_hz: u32,
    pclka_hz: u32,
    mode: u8,
    lsb_first: bool,
    loopback: bool,
};

/// SPCMD0: CPHA/CPOL from the SPI mode, LSBF, 8-bit frames.
pub fn spcmd(cfg: *const Cfg) u32 {
    var v: u32 = switch (cfg.mode) {
        1 => 0x1,
        2 => 0x2,
        3 => 0x3,
        else => 0,
    };
    if (cfg.lsb_first) v |= 0x1000;
    return v | (7 << 16);
}

/// ra8_hw_wait_flag_set32: up to `poll_limit` reads, then hw_timeout.
fn waitSet(regs: anytype, channel: u8, mask: u32) u16 {
    for (0..poll_limit) |_| {
        if (regs.read32(channel, off_spsr) & mask != 0) return ok;
    }
    return err_hw_timeout;
}

fn program(regs: anytype, ch: u8, cfg: *const Cfg) void {
    regs.write32(ch, off_spsrc, spsrc_all);
    regs.write32(ch, off_spcr3, 0);
    regs.write32(ch, off_spdecr, 0);
    regs.write32(ch, off_spcr2, 0);
    regs.write32(ch, off_spcmd0, spcmd(cfg));
    regs.write32(ch, off_spdcr, 0);
    regs.write32(ch, off_spdcr2, 0);
    regs.write32(ch, off_spfcr, spfcr_spfrst);
    regs.write32(ch, off_spsrc, spsrc_all);
}

pub fn init(regs: anytype, c: anytype, channel: u8, cfg: ?*const Cfg) u16 {
    const k = cfg orelse {
        c.err("target_init: cfg");
        return err_null_ptr;
    };
    if (channel >= channel_count) return err_invalid_arg;
    const err = c.mstpEnable(mstpId(channel));
    if (err != ok) {
        c.fail("target_init: mstp", err);
        return err;
    }
    regs.write32(channel, off_spcr, 0);
    program(regs, channel, k);
    regs.write32(channel, off_spcr, spcr_target);
    c.infoVal("target_init channel", channel);
    return ok;
}

pub fn xfer(regs: anytype, c: anytype, channel: u8, tx: u8, rx: ?*u8) u16 {
    if (channel >= channel_count) {
        c.err("target_xfer: channel out of range");
        return err_null_ptr;
    }
    var err = waitSet(regs, channel, spsr_sptef);
    if (err != ok) return err;
    regs.write32(channel, off_spdr, tx);
    regs.write32(channel, off_spsrc, spsr_sptef);
    err = waitSet(regs, channel, spsr_sprf);
    if (err != ok) return err;
    const received: u8 = @truncate(regs.read32(channel, off_spdr));
    regs.write32(channel, off_spsrc, spsr_sprf);
    if (rx) |p| p.* = received;
    return ok;
}
