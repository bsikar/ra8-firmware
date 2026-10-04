//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! MIPI CSI-2 receiver option setters (RA8FW-634): data-type filter, ECC and
//! frame-error modes, EPD spacers and LRTE, ported from ra8_mipi_csi.c.
//! Registers are reached through a `csi` ops value (offsets from the CSI
//! base) so host tests can use a fake register file.

pub const ok: u16 = 0;
pub const invalid_arg: u16 = 0x103;
pub const invalid_state: u16 = 0x104;

pub const off_mct0: u16 = 0x010;
pub const off_mct3: u16 = 0x01C;
pub const off_epct: u16 = 0x040;
pub const off_emct: u16 = 0x044;
pub const off_dtel: u16 = 0x060;
pub const off_dteh: u16 = 0x064;

pub const mct3_rxen: u32 = 0x0000_0001;
pub const mct0_zlmd: u32 = 0x0001_0000;
pub const mct0_edmd: u32 = 0x0002_0000;
pub const mct0_rvmd: u32 = 0x0008_0000;
pub const mct0_eccv13: u32 = 0x0100_0000;
pub const mct0_lfsren: u32 = 0x0200_0000;
pub const epct_slp: u32 = 0x0000_7FFF;
pub const epct_ssp: u32 = 0x7FFF_0000;
pub const epct_ssp_shift: u5 = 16;
pub const epct_epdop: u32 = 0x0000_8000;
pub const epct_epden: u32 = 0x8000_0000;
pub const emct_vlsien: u32 = 0x0000_0030;
pub const emct_vlsien_shift: u5 = 4;
pub const emct_eotpen: u32 = 0x0000_0040;
pub const vlsien_max: u8 = 3;

fn bit(on: bool, mask: u32) u32 {
    return if (on) mask else 0;
}

/// Refuse a write that needs RXEN = 0; logs like RA8_RETURN_ON_ERROR.
fn rejectIfRunning(csi: anytype, msg: [*:0]const u8) u16 {
    if (csi.read32(off_mct3) & mct3_rxen == 0) return ok;
    csi.errVal(msg, invalid_state);
    return invalid_state;
}

pub fn setDataTypeFilter(csi: anytype, low_mask: u32, high_mask: u32) u16 {
    csi.write32(off_dtel, low_mask);
    csi.write32(off_dteh, high_mask);
    return ok;
}

fn updateMct0(csi: anytype, clear: u32, set: u32) void {
    const mct0 = csi.read32(off_mct0);
    csi.write32(off_mct0, (mct0 & ~clear) | set);
}

pub fn setEccMode(csi: anytype, eccv13: bool, lfsren: bool) u16 {
    const rc = rejectIfRunning(csi, "set_ecc_mode: rxen set");
    if (rc != ok) return rc;
    updateMct0(csi, mct0_eccv13 | mct0_lfsren, bit(eccv13, mct0_eccv13) | bit(lfsren, mct0_lfsren));
    return ok;
}

pub fn setFrameErrorMode(csi: anytype, zlmd: bool, edmd: bool, rvmd: bool) u16 {
    const rc = rejectIfRunning(csi, "set_frame_error_mode: rxen set");
    if (rc != ok) return rc;
    const set = bit(zlmd, mct0_zlmd) | bit(edmd, mct0_edmd) | bit(rvmd, mct0_rvmd);
    updateMct0(csi, mct0_zlmd | mct0_edmd | mct0_rvmd, set);
    return ok;
}

pub fn setEpd(csi: anytype, enable: bool, option_2: bool, long_spacer: u16, short_spacer: u16) u16 {
    const ssp_field = epct_ssp >> epct_ssp_shift;
    if (@as(u32, long_spacer) & ~epct_slp != 0) return invalid_arg;
    if (@as(u32, short_spacer) & ~ssp_field != 0) return invalid_arg;
    const rc = rejectIfRunning(csi, "set_epd: rxen set");
    if (rc != ok) return rc;
    var epct: u32 = (@as(u32, long_spacer) & epct_slp) | ((@as(u32, short_spacer) & ssp_field) << epct_ssp_shift);
    epct |= bit(option_2, epct_epdop) | bit(enable, epct_epden);
    csi.write32(off_epct, epct);
    return ok;
}

pub fn setLrte(csi: anytype, vlsien: u8, eotp_enable: bool) u16 {
    if (vlsien > vlsien_max) return invalid_arg;
    const rc = rejectIfRunning(csi, "set_lrte: rxen set");
    if (rc != ok) return rc;
    var emct = csi.read32(off_emct) & ~(emct_vlsien | emct_eotpen);
    emct |= (@as(u32, vlsien) << emct_vlsien_shift) & emct_vlsien;
    emct |= bit(eotp_enable, emct_eotpen);
    csi.write32(off_emct, emct);
    return ok;
}
