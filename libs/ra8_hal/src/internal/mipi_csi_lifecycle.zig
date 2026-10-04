//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! MIPI CSI-2 receiver lifecycle (RA8FW-641): init, deinit, reset and
//! start/stop receive, the last part of ra8_mipi_csi.c. Registers, module
//! stop, logging and handler detach go through a `csi` ops value so host
//! tests can use a fake register file.

pub const ok: u16 = 0;
pub const invalid_arg: u16 = 0x103;
pub const invalid_state: u16 = 0x104;
pub const hw_timeout: u16 = 0x203;
pub const null_ptr: u16 = 0x504;

pub const off_mct0: u16 = 0x010;
pub const off_mct2: u16 = 0x018;
pub const off_mct3: u16 = 0x01C;
pub const off_rtct: u16 = 0x028;
pub const off_rtst: u16 = 0x02C;
pub const off_epct: u16 = 0x040;
pub const off_emct: u16 = 0x044;
pub const off_dtel: u16 = 0x060;
pub const off_dteh: u16 = 0x064;
pub const off_rxie: u16 = 0x078;
pub const off_dlie0: u16 = 0x088;
pub const off_dlie1: u16 = 0x098;
pub const off_vcie0: u16 = 0x108;
pub const vc_stride: u16 = 0x10;
pub const vc_count: u8 = 16;
pub const off_pmie: u16 = 0x208;
pub const off_gsct: u16 = 0x280;
pub const off_gsie: u16 = 0x28C;

pub const mct3_rxen: u32 = 0x0000_0001;
pub const rtct_vsrst: u32 = 0x0000_0001;
pub const rtst_vsrsts: u32 = 0x0000_0001;
pub const mct0_vdln: u32 = 0x0000_000F;
pub const mct0_zlmd: u32 = 0x0001_0000;
pub const mct0_edmd: u32 = 0x0002_0000;
pub const mct0_rvmd: u32 = 0x0008_0000;
pub const mct0_grmd: u32 = 0x0010_0000;
pub const mct0_eccv13: u32 = 0x0100_0000;
pub const mct0_lfsren: u32 = 0x0200_0000;
pub const mct2_field: u32 = 0x0000_01FF;
pub const mct2_frrskw_shift: u5 = 16;
pub const epct_field: u32 = 0x0000_7FFF;
pub const epct_ssp_shift: u5 = 16;
pub const epct_epdop: u32 = 0x0000_8000;
pub const epct_epden: u32 = 0x8000_0000;
pub const emct_vlsien: u32 = 0x0000_0030;
pub const emct_vlsien_shift: u5 = 4;
pub const emct_eotpen: u32 = 0x0000_0040;
pub const gsct_shth: u32 = 0x0000_007F;
pub const gsct_gfif: u32 = 0x0001_0000;
pub const shth_max: u8 = 0x0F;
pub const vlsien_max: u8 = 3;
pub const reset_spin_max: u16 = 1024;

/// Mirrors `ra8_mipi_csi_config_t` (inc/ra8_mipi_csi_types.h); enums are
/// carried as their u8 storage so out-of-range values reach validation.
pub const Config = extern struct {
    lanes: u8,
    generic_rule: bool,
    eccv13: bool,
    lfsren: bool,
    zlmd: bool,
    edmd: bool,
    rvmd: bool,
    frrclk: u16,
    frrskw: u16,
    epd_enable: bool,
    epd_option_2: bool,
    epd_long_spacer: u16,
    epd_short_spacer: u16,
    vlsien: u8,
    eotp_enable: bool,
    dt_low_mask: u32,
    dt_high_mask: u32,
    rx_irq_mask: u32,
    dl_irq_mask: [2]u32,
    vc_irq_mask: [16]u32,
    pm_irq_mask: u32,
    short_irq_mask: u32,
    short_threshold: u8,
    short_store_enable: bool,
};

comptime {
    if (@sizeOf(Config) != 116) @compileError("Config must match ra8_mipi_csi_config_t (116 B)");
    if (@offsetOf(Config, "vc_irq_mask") != 40) @compileError("vc_irq_mask offset");
    if (@offsetOf(Config, "short_threshold") != 112) @compileError("short_threshold offset");
}

fn bit(on: bool, mask: u32) u32 {
    return if (on) mask else 0;
}

pub fn validate(cfg: *const Config) u16 {
    if (cfg.lanes != 1 and cfg.lanes != 2) return invalid_arg;
    if (cfg.vlsien > vlsien_max) return invalid_arg;
    if (@as(u32, cfg.epd_long_spacer) & ~epct_field != 0) return invalid_arg;
    if (@as(u32, cfg.epd_short_spacer) & ~epct_field != 0) return invalid_arg;
    if (cfg.short_threshold > shth_max) return invalid_arg;
    return ok;
}

pub fn encodeMct0(cfg: *const Config) u32 {
    return (@as(u32, cfg.lanes) & mct0_vdln) |
        bit(cfg.generic_rule, mct0_grmd) |
        bit(cfg.eccv13, mct0_eccv13) |
        bit(cfg.lfsren, mct0_lfsren) |
        bit(cfg.zlmd, mct0_zlmd) |
        bit(cfg.edmd, mct0_edmd) |
        bit(cfg.rvmd, mct0_rvmd);
}

pub fn programReceiver(csi: anytype, cfg: *const Config) void {
    csi.write32(off_mct0, encodeMct0(cfg));
    const mct2 = ((@as(u32, cfg.frrskw) & mct2_field) << mct2_frrskw_shift) |
        (@as(u32, cfg.frrclk) & mct2_field);
    csi.write32(off_mct2, mct2);
    const epct = (@as(u32, cfg.epd_long_spacer) & epct_field) |
        ((@as(u32, cfg.epd_short_spacer) & epct_field) << epct_ssp_shift) |
        bit(cfg.epd_option_2, epct_epdop) | bit(cfg.epd_enable, epct_epden);
    csi.write32(off_epct, epct);
    const emct = ((@as(u32, cfg.vlsien) << emct_vlsien_shift) & emct_vlsien) |
        bit(cfg.eotp_enable, emct_eotpen);
    csi.write32(off_emct, emct);
    csi.write32(off_dtel, cfg.dt_low_mask);
    csi.write32(off_dteh, cfg.dt_high_mask);
    const gsct = (@as(u32, cfg.short_threshold) & gsct_shth) |
        bit(cfg.short_store_enable, gsct_gfif);
    csi.write32(off_gsct, gsct);
}

/// Writes RXIE, DLIE0/1, VCIE0..15, PMIE and GSIE; `cfg` null clears them.
pub fn programIrqMasks(csi: anytype, cfg: ?*const Config) void {
    csi.write32(off_rxie, if (cfg) |c| c.rx_irq_mask else 0);
    csi.write32(off_dlie0, if (cfg) |c| c.dl_irq_mask[0] else 0);
    csi.write32(off_dlie1, if (cfg) |c| c.dl_irq_mask[1] else 0);
    var vc: u8 = 0;
    while (vc < vc_count) : (vc += 1) {
        const off = off_vcie0 + @as(u16, vc) * vc_stride;
        csi.write32(off, if (cfg) |c| c.vc_irq_mask[vc] else 0);
    }
    csi.write32(off_pmie, if (cfg) |c| c.pm_irq_mask else 0);
    csi.write32(off_gsie, if (cfg) |c| c.short_irq_mask else 0);
}

pub fn waitResetIdle(csi: anytype) u16 {
    var i: u16 = 0;
    while (i < reset_spin_max) : (i += 1) {
        if (csi.read32(off_rtst) & rtst_vsrsts == 0) return ok;
    }
    return hw_timeout;
}

pub fn init(csi: anytype, maybe_cfg: ?*const Config) u16 {
    const cfg = maybe_cfg orelse {
        csi.err("cfg must not be nullptr");
        return null_ptr;
    };
    const cfg_rc = validate(cfg);
    if (cfg_rc != ok) return fail(csi, "mipi_csi_init: cfg out of range", cfg_rc);
    const mst_rc = csi.mstpEnable();
    if (mst_rc != ok) return fail(csi, "mipi_csi_init: mstp enable", mst_rc);
    csi.write32(off_mct3, 0);
    const rst_rc = waitResetIdle(csi);
    if (rst_rc != ok) return fail(csi, "mipi_csi_init: vsrsts spin", rst_rc);
    programReceiver(csi, cfg);
    programIrqMasks(csi, cfg);
    csi.infoVal("mipi_csi_init lanes", cfg.lanes);
    return ok;
}

fn fail(csi: anytype, msg: [*:0]const u8, rc: u16) u16 {
    csi.errVal(msg, rc);
    return rc;
}

pub fn deinit(csi: anytype) u16 {
    csi.write32(off_mct3, 0);
    csi.write32(off_rtct, rtct_vsrst);
    programIrqMasks(csi, null);
    csi.detachAll();
    return csi.mstpDisable();
}

pub fn reset(csi: anytype) u16 {
    csi.write32(off_rtct, rtct_vsrst);
    return waitResetIdle(csi);
}

pub fn startReceive(csi: anytype) u16 {
    if (csi.read32(off_mct3) & mct3_rxen != 0) {
        return fail(csi, "mipi_csi start: already running", invalid_state);
    }
    csi.write32(off_mct3, mct3_rxen);
    csi.info("mipi_csi start_receive");
    return ok;
}

pub fn stopReceive(csi: anytype) u16 {
    csi.write32(off_mct3, 0);
    csi.write32(off_rtct, rtct_vsrst);
    csi.info("mipi_csi stop_receive");
    return ok;
}
