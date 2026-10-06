//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! SPI_B init-time register packing (RA8FW-898, was part of ra8_spi_b.c).
//! HUM Ch 43.2.4 "SPCR" p 2884, 43.2.5 "SPCR2" p 2889, 43.2.7 "SPCMDm" p 2893.
//! Exports live in src/spi_b_setup_abi.zig.

/// Mirrors `ra8_spi_cfg_t` in ra8_spi.h field for field.
pub const Cfg = extern struct {
    baud_hz: u32,
    pclka_hz: u32,
    mode: u8,
    lsb_first: bool,
    loopback: bool,
};

pub const default_cfg = Cfg{
    .baud_hz = 1_900_000,
    .pclka_hz = 125_000_000,
    .mode = 0,
    .lsb_first = false,
    .loopback = false,
};

pub const spcmd_cpha: u32 = 0x0000_0001;
pub const spcmd_cpol: u32 = 0x0000_0002;
pub const spcmd_lsbf: u32 = 0x0000_1000;
pub const spcmd_spb_mask: u32 = 0x001F_0000;
pub const spcmd_spb_shift = 16;
pub const spb_8bit: u32 = 0x07;

pub const spcr_spe: u32 = 0x0000_0001;
pub const spcr_sckase: u32 = 0x0000_1000;
pub const spcr_mstr: u32 = 0x4000_0000;
pub const spcr2_splp2: u32 = 0x0002_0000;
pub const spfcr_spfrst: u32 = 0x0000_0001;
pub const spsrc_all: u32 = 0xFD80_0000;

/// SPCMD0: CPHA for modes 1/3, CPOL for modes 2/3, LSBF, 8-bit frame.
pub fn spcmd(cfg: Cfg) u32 {
    var v: u32 = 0;
    if (cfg.mode == 1 or cfg.mode == 3) v |= spcmd_cpha;
    if (cfg.mode == 2 or cfg.mode == 3) v |= spcmd_cpol;
    if (cfg.lsb_first) v |= spcmd_lsbf;
    return v | ((spb_8bit << spcmd_spb_shift) & spcmd_spb_mask);
}

/// SPCR for an enabled controller with auto-stopped SCK.
pub fn spcrController() u32 {
    return spcr_mstr | spcr_sckase | spcr_spe;
}

/// SPCR2: SPLP2 is the non-inverting internal loopback (rx = tx).
pub fn spcr2(cfg: Cfg) u32 {
    return if (cfg.loopback) spcr2_splp2 else 0;
}
