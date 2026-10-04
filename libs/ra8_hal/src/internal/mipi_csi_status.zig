//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! MIPI CSI-2 lane, virtual-channel, power-management and generic
//! short-packet status/control (RA8FW-632), ported from the tail of
//! ra8_mipi_csi.c. Registers are reached through a `csi` ops value
//! (offsets from the CSI base) so host tests can use a fake register file.

pub const ok: u16 = 0;
pub const invalid_arg: u16 = 0x103;
pub const empty: u16 = 0x10D;
pub const hw_timeout: u16 = 0x203;
pub const null_ptr: u16 = 0x504;

pub const off_dlst0: u16 = 0x080;
pub const off_dlsc0: u16 = 0x084;
pub const off_dlie0: u16 = 0x088;
pub const off_vcst0: u16 = 0x100;
pub const off_vcsc0: u16 = 0x104;
pub const off_vcie0: u16 = 0x108;
pub const off_pmst: u16 = 0x200;
pub const off_pmsc: u16 = 0x204;
pub const off_pmie: u16 = 0x208;
pub const off_gsct: u16 = 0x280;
pub const off_gsst: u16 = 0x284;
pub const off_gssc: u16 = 0x288;
pub const off_gsie: u16 = 0x28C;
pub const off_gsht: u16 = 0x290;
pub const off_gsiu: u16 = 0x294;

pub const dl_max: u8 = 1;
pub const vc_max: u8 = 15;
pub const stride: u16 = 0x10;

pub const dlsc_all: u32 = 0x0003_000F;
pub const pm_all: u32 = 0x0000_00FF;
pub const gsct_shth_max: u32 = 0x0F;
pub const gsct_shth_mask: u32 = 0x7F;
pub const gsct_gfif: u32 = 0x0001_0000;
pub const gsie_all: u32 = 0x13;
pub const gssc_govc: u32 = 0x10;
pub const gsst_pnum_mask: u32 = 0x0000_FF00;
pub const gsst_gcd: u32 = 0x0001_0000;
pub const gsiu_finc: u32 = 0x0000_0001;
pub const gsiu_gfclr: u32 = 0x0000_0100;
pub const gsiu_gfen: u32 = 0x0001_0000;
pub const gfclr_spin_max: u16 = 1024;

/// Mirrors ra8_mipi_csi_short_packet_t (ra8_mipi_csi_types.h).
pub const ShortPacket = extern struct {
    payload: u16,
    data_type: u8,
    vc: u8,
    raw: u32,
};

/// Which per-index register block a status call targets.
pub const Block = enum { lane, vc };

fn blockOff(block: Block, base: u16, idx: u8) ?u16 {
    const max = switch (block) {
        .lane => dl_max,
        .vc => vc_max,
    };
    if (idx > max) return null;
    return base + @as(u16, idx) * stride;
}

pub fn getIndexed(csi: anytype, block: Block, base: u16, idx: u8, out: ?*u32) u16 {
    const dst = out orelse {
        csi.err("out_mask must not be nullptr");
        return null_ptr;
    };
    const off = blockOff(block, base, idx) orelse return invalid_arg;
    dst.* = csi.read32(off);
    return ok;
}

pub fn writeIndexed(csi: anytype, block: Block, base: u16, idx: u8, value: u32) u16 {
    const off = blockOff(block, base, idx) orelse return invalid_arg;
    csi.write32(off, value);
    return ok;
}

pub fn getReg(csi: anytype, off: u16, out: ?*u32) u16 {
    const dst = out orelse {
        csi.err("out_mask must not be nullptr");
        return null_ptr;
    };
    dst.* = csi.read32(off);
    return ok;
}

pub fn configureShortPacket(csi: anytype, threshold: u8, store_enable: bool) u16 {
    if (@as(u32, threshold) > gsct_shth_max) return invalid_arg;
    var gsct: u32 = @as(u32, threshold) & gsct_shth_mask;
    if (store_enable) gsct |= gsct_gfif;
    csi.write32(off_gsct, gsct);
    return ok;
}

pub fn readShortPacket(csi: anytype, out: ?*ShortPacket) u16 {
    const dst = out orelse {
        csi.err("out must not be nullptr");
        return null_ptr;
    };
    const gsst = csi.read32(off_gsst);
    if ((gsst & gsst_pnum_mask) >> 8 == 0) return empty;
    csi.write32(off_gsiu, gsiu_finc);
    const gsht = csi.read32(off_gsht);
    dst.* = .{
        .payload = @truncate(gsht & 0xFFFF),
        .data_type = @truncate((gsht >> 16) & 0x3F),
        .vc = @truncate((gsht >> 24) & 0x0F),
        .raw = gsht,
    };
    return ok;
}

pub fn clearFifo(csi: anytype) u16 {
    csi.write32(off_gsiu, gsiu_gfclr);
    var result: u16 = hw_timeout;
    var i: u16 = 0;
    while (i < gfclr_spin_max) : (i += 1) {
        if (csi.read32(off_gsst) & gsst_gcd != 0) {
            result = ok;
            break;
        }
    }
    // Release the request line regardless of outcome.
    csi.write32(off_gsiu, 0);
    return result;
}
