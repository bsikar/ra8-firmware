//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the ETHA queue and VLAN configuration entry points (RA8FW-814).
//! Logic is in internal/etha_cfg.zig; pointer checks come first, then the
//! port and argument checks, as the C did.

const common = @import("abi_common.zig");
const cfg = @import("internal/etha_cfg.zig");

const tag = "ETHA";
const port_bases = [_]usize{ 0x403C_A000, 0x403C_C000 };

comptime {
    if (@sizeOf(cfg.VlanTag) != 4) @compileError("ra8_etha_vlan_tag_t is 4 bytes");
}

const Mmio = struct {
    base: usize,
    pub fn read32(self: Mmio, off: usize) u32 {
        const p: *volatile u32 = @ptrFromInt(self.base + off);
        return p.*;
    }
    pub fn write32(self: Mmio, off: usize, v: u32) void {
        const p: *volatile u32 = @ptrFromInt(self.base + off);
        p.* = v;
    }
};

fn fail(code: u16, msg: [*:0]const u8) u16 {
    common.ra8_log_emit_error(tag, msg);
    return code;
}

fn regs(p: u8) Mmio {
    return .{ .base = port_bases[p] };
}

fn portOk(p: u8) bool {
    return p < port_bases.len;
}

export fn ra8_etha_set_queue_arb(p: u8, tc: u8, arb: u8) u16 {
    if (!portOk(p) or !cfg.tcOk(tc) or arb > cfg.mask_tdqa) return fail(common.k_ra8_err_invalid_arg, "etha_set_queue_arb: bad arg");
    cfg.setQueueArb(regs(p), tc, arb);
    return common.k_ra8_ok;
}

export fn ra8_etha_set_queue_depth(p: u8, tc: u8, depth: u16) u16 {
    if (!portOk(p) or !cfg.tcOk(tc) or depth > cfg.mask_dqd) return fail(common.k_ra8_err_invalid_arg, "etha_set_queue_depth: bad arg");
    regs(p).write32(cfg.off_eatdqdc + 4 * @as(usize, tc), depth & cfg.mask_dqd);
    return common.k_ra8_ok;
}

export fn ra8_etha_get_queue_level(p: u8, tc: u8, cur_level: ?*u16, peak: ?*u16) u16 {
    const cur = cur_level orelse return fail(common.k_ra8_err_null_ptr, "etha_get_queue_level: cur_level null");
    const pk = peak orelse return fail(common.k_ra8_err_null_ptr, "etha_get_queue_level: peak null");
    if (!portOk(p) or !cfg.tcOk(tc)) return fail(common.k_ra8_err_invalid_arg, "etha_get_queue_level: bad arg");
    const level = cfg.queueLevel(regs(p), tc);
    cur.* = level[0];
    pk.* = level[1];
    return common.k_ra8_ok;
}

export fn ra8_etha_set_preemption(p: u8, preempt: u8, cut_thru: u8, afs: u8) u16 {
    if (!portOk(p)) return fail(common.k_ra8_err_invalid_arg, "etha_set_preemption: port out of range");
    regs(p).write32(cfg.off_eatpec, cfg.preemption(preempt, cut_thru, afs));
    return common.k_ra8_ok;
}

export fn ra8_etha_set_max_frame_size(p: u8, tc: u8, max_bytes: u16) u16 {
    if (!portOk(p) or !cfg.tcOk(tc)) return fail(common.k_ra8_err_invalid_arg, "etha_set_max_frame_size: bad arg");
    regs(p).write32(cfg.off_eatmfsc + 4 * @as(usize, tc), max_bytes & cfg.mask_mfs);
    return common.k_ra8_ok;
}

export fn ra8_etha_set_ipv_remap(p: u8, map: ?*const [cfg.tc_count]u8) u16 {
    const m = map orelse return fail(common.k_ra8_err_null_ptr, "etha_set_ipv_remap: map null");
    if (!portOk(p)) return fail(common.k_ra8_err_invalid_arg, "etha_set_ipv_remap: port out of range");
    if (!cfg.ipvMapOk(m)) return fail(common.k_ra8_err_invalid_arg, "etha_set_ipv_remap: entry > 7");
    regs(p).write32(cfg.off_eairc, cfg.ipvPack(m));
    return common.k_ra8_ok;
}

export fn ra8_etha_set_vlan_mode(p: u8, vim: u8, vem: u8) u16 {
    if (!portOk(p)) return fail(common.k_ra8_err_invalid_arg, "etha_set_vlan_mode: port out of range");
    regs(p).write32(cfg.off_eavcc, cfg.vlanMode(vim, vem));
    return common.k_ra8_ok;
}

export fn ra8_etha_set_vlan_tag(p: u8, c_tag: ?*const cfg.VlanTag, s_tag: ?*const cfg.VlanTag) u16 {
    const c = c_tag orelse return fail(common.k_ra8_err_null_ptr, "etha_set_vlan_tag: c_tag null");
    const s = s_tag orelse return fail(common.k_ra8_err_null_ptr, "etha_set_vlan_tag: s_tag null");
    if (!portOk(p)) return fail(common.k_ra8_err_invalid_arg, "etha_set_vlan_tag: port out of range");
    if (!cfg.tagOk(c) or !cfg.tagOk(s)) return fail(common.k_ra8_err_invalid_arg, "etha_set_vlan_tag: tag fields out of range");
    regs(p).write32(cfg.off_eavtc, cfg.vlanTag(c, s));
    return common.k_ra8_ok;
}

export fn ra8_etha_set_rx_tag_filter(p: u8, mask: u32) u16 {
    if (!portOk(p)) return fail(common.k_ra8_err_invalid_arg, "etha_set_rx_tag_filter: port out of range");
    regs(p).write32(cfg.off_eartfc, mask & cfg.mask_rx_tag);
    return common.k_ra8_ok;
}

export fn ra8_etha_configure_cut_through(p: u8, qd: u16, dqd: u8) u16 {
    if (!portOk(p) or dqd > cfg.mask_ctdqd) return fail(common.k_ra8_err_invalid_arg, "etha_configure_cut_through: bad arg");
    const r = regs(p);
    r.write32(cfg.off_eactqc, qd & cfg.mask_ctqd);
    r.write32(cfg.off_eactdqdc, dqd & cfg.mask_ctdqd);
    return common.k_ra8_ok;
}
