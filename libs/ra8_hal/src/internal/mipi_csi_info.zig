//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! MIPI CSI-2 receive-status and module-info accessors (RA8FW-636), ported
//! from ra8_mipi_csi.c. Registers are reached through a `csi` ops value
//! (offsets from the CSI base) so host tests can use a fake register file.

pub const ok: u16 = 0;
pub const null_ptr: u16 = 0x504;

pub const off_mcg: u16 = 0x000;
pub const off_mist: u16 = 0x050;
pub const off_rxst: u16 = 0x070;
pub const off_rxsc: u16 = 0x074;
pub const off_rxie: u16 = 0x078;

pub const mcg_ver: u32 = 0x0000_000F;
pub const mcg_sdln: u32 = 0x0000_0F00;
pub const mcg_sdln_shift: u5 = 8;
pub const mcg_gsnm: u32 = 0x00FF_0000;
pub const mcg_gsnm_shift: u5 = 16;

/// Mirrors ra8_mipi_csi_module_info_t (ra8_mipi_csi_types.h).
pub const ModuleInfo = extern struct {
    version: u8,
    lanes_max: u8,
    fifo_stages: u8,
    raw: u32,
};

pub fn readReg(csi: anytype, off: u16, out: ?*u32) u16 {
    const dst = out orelse {
        csi.err("out_mask must not be nullptr");
        return null_ptr;
    };
    dst.* = csi.read32(off);
    return ok;
}

pub fn decodeMcg(mcg: u32) ModuleInfo {
    return .{
        .version = @truncate(mcg & mcg_ver),
        .lanes_max = @truncate((mcg & mcg_sdln) >> mcg_sdln_shift),
        .fifo_stages = @truncate((mcg & mcg_gsnm) >> mcg_gsnm_shift),
        .raw = mcg,
    };
}

pub fn getModuleInfo(csi: anytype, out: ?*ModuleInfo) u16 {
    const dst = out orelse {
        csi.err("out must not be nullptr");
        return null_ptr;
    };
    dst.* = decodeMcg(csi.read32(off_mcg));
    return ok;
}
