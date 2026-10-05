//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! SD card protocol over SDHI (RA8FW-791), from ra8_sdcard.c: CSD decode,
//! card classification, addressing, and the identify, ACMD41 and
//! publish/select command sequences. Generic over a host with
//! send(cmd, arg, rsp) u16 and failed(err, msg) bool (logs on error).

pub const ok: u16 = 0;
pub const err_hw_init_failed: u16 = 0x201;

pub const type_unknown: u8 = 0;
pub const type_sdsc: u8 = 1;
pub const type_sdhc: u8 = 2;
pub const type_sdxc: u8 = 3;

pub const cmd0_go_idle: u32 = 0;
pub const cmd2_all_send_cid: u32 = 2;
pub const cmd3_send_rca: u32 = 3;
pub const cmd7_select: u32 = 7;
pub const cmd8_send_if_cond: u32 = 8;
pub const cmd9_send_csd: u32 = 9;
pub const cmd55_app: u32 = 55;
pub const acmd41_op_cond: u32 = 41;

pub const cmd8_pattern: u32 = 0x1AA;
pub const cmd8_mask: u32 = 0xFFF;
/// HCS plus the 2.7 to 3.6 V window.
pub const acmd41_arg: u32 = 0x4000_0000 | 0x00FF_8000;
pub const ocr_busy_done: u32 = 0x8000_0000;
pub const ocr_ccs: u32 = 0x4000_0000;
pub const retry_max: u32 = 1000;
pub const default_clk_div: u32 = 4;
pub const block_size: u32 = 512;
pub const sdhc_threshold_blocks: u32 = 4_194_304;

/// CSD v2 counts 512 KiB units; v1 is converted to 512-byte sectors.
pub fn decodeCsd(rsp: *const [4]u32, out_blocks: *u32) u16 {
    const structure = (rsp[3] >> 30) & 0x3;
    if (structure == 1) {
        const c_size = (((rsp[2] & 0x3F) << 16) | ((rsp[1] >> 16) & 0xFFFF)) & 0x3F_FFFF;
        out_blocks.* = (c_size + 1) *% 1024;
        return ok;
    }
    if (structure == 0) {
        const read_bl_len: u5 = @intCast((rsp[2] >> 16) & 0xF);
        const c_size = (((rsp[2] & 0x3) << 10) | ((rsp[1] >> 22) & 0x3FF)) & 0xFFF;
        const mult_shift: u5 = @intCast(((rsp[1] >> 7) & 0x7) + 2);
        const blocknr = (c_size + 1) * (@as(u32, 1) << mult_shift);
        out_blocks.* = (blocknr *% (@as(u32, 1) << read_bl_len)) / block_size;
        return ok;
    }
    return err_hw_init_failed;
}

pub fn classify(high_capacity: bool, blocks: u32) u8 {
    if (!high_capacity) return type_sdsc;
    if (blocks > sdhc_threshold_blocks * 16) return type_sdxc;
    return type_sdhc;
}

/// SDSC cards are byte-addressed.
pub fn cardAddress(kind: u8, lba: u32) u32 {
    return if (kind == type_sdsc) lba *% block_size else lba;
}

/// CMD0, then CMD8 with the low 12 bits echoed back.
pub fn identify(host: anytype) u16 {
    var rsp = [_]u32{0} ** 4;
    const e0 = host.send(cmd0_go_idle, 0, &rsp);
    if (host.failed(e0, "cmd0")) return e0;
    const e8 = host.send(cmd8_send_if_cond, cmd8_pattern, &rsp);
    if (host.failed(e8, "cmd8")) return e8;
    if (rsp[0] & cmd8_mask != cmd8_pattern & cmd8_mask) return err_hw_init_failed;
    return ok;
}

/// CMD55 + ACMD41 until OCR.busy sets, at most retry_max rounds.
pub fn acmd41(host: anytype, out_ocr: *u32) u16 {
    var rsp = [_]u32{0} ** 4;
    var i: u32 = 0;
    while (i < retry_max) : (i += 1) {
        const e55 = host.send(cmd55_app, 0, &rsp);
        if (e55 != ok) return e55;
        const e41 = host.send(acmd41_op_cond, acmd41_arg, &rsp);
        if (e41 != ok) return e41;
        if (rsp[0] & ocr_busy_done != 0) {
            out_ocr.* = rsp[0];
            return ok;
        }
    }
    return err_hw_init_failed;
}

/// CMD2, CMD3 (RCA in rsp0[31:16]).
fn publishRca(host: anytype, out_rca: *u16) u16 {
    var rsp = [_]u32{0} ** 4;
    const e2 = host.send(cmd2_all_send_cid, 0, &rsp);
    if (host.failed(e2, "cmd2")) return e2;
    const e3 = host.send(cmd3_send_rca, 0, &rsp);
    if (host.failed(e3, "cmd3")) return e3;
    out_rca.* = @truncate(rsp[0] >> 16);
    return ok;
}

/// RCA, CMD9 (CSD) and decode, then CMD7 to put the card in TRAN.
pub fn publishAndSelect(host: anytype, out_rca: *u16, out_blocks: *u32) u16 {
    var rca: u16 = 0;
    const rca_err = publishRca(host, &rca);
    if (rca_err != ok) return rca_err;
    const rca_arg = @as(u32, rca) << 16;
    var rsp = [_]u32{0} ** 4;
    const e9 = host.send(cmd9_send_csd, rca_arg, &rsp);
    if (host.failed(e9, "cmd9")) return e9;
    const dec = decodeCsd(&rsp, out_blocks);
    if (dec != ok) return dec;
    const e7 = host.send(cmd7_select, rca_arg, &rsp);
    if (host.failed(e7, "cmd7")) return e7;
    out_rca.* = rca;
    return ok;
}
