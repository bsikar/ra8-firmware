//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the ETHA ring/stats/open entry points (RA8FW-591). The per-port
//! slots are defined in etha_life_abi.zig; the logic is in internal/etha_stats.zig.

const common = @import("abi_common.zig");
const st = @import("internal/etha_stats.zig");

const tag = "ETHA";
const port_bases = [_]usize{ 0x403C_A000, 0x403C_C000 };

/// `ra8_etha_slot_t`; only `stats` is touched here.
const Slot = extern struct { cb: ?*const anyopaque, ctx: ?*anyopaque, stats: st.Stats };
/// `ra8_etha_ring_cfg_t`.
const RingCfg = extern struct { num_tx: u16, num_rx: u16, buffer_size: u16 };

extern var s_etha_slots: [port_bases.len]Slot;
extern fn ra8_rmac_phy_reset(port: u8, phy_addr: u8) u16;
extern fn ra8_rmac_phy_set_advertise(port: u8, phy_addr: u8, caps: u16) u16;
extern fn ra8_rmac_phy_auto_neg_start(port: u8, phy_addr: u8) u16;
extern fn ra8_rmac_phy_auto_neg_wait(port: u8, phy_addr: u8, timeout_ms: u32, out: *anyopaque) u16;

comptime {
    if (@sizeOf(st.Stats) != 28) @compileError("ra8_etha_port_stats_t is 28 bytes");
    if (@offsetOf(st.PhyOpen, "timeout_ms") != 4) @compileError("phy_open.timeout_ms sits at +4");
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

const C = struct {
    pub fn logError(_: C, msg: [*:0]const u8) void {
        common.ra8_log_emit_error(tag, msg);
    }
    pub fn phyReset(_: C, port: u8, addr: u8) u16 {
        return ra8_rmac_phy_reset(port, addr);
    }
    pub fn phySetAdvertise(_: C, port: u8, addr: u8, caps: u16) u16 {
        return ra8_rmac_phy_set_advertise(port, addr, caps);
    }
    pub fn phyAutoNegStart(_: C, port: u8, addr: u8) u16 {
        return ra8_rmac_phy_auto_neg_start(port, addr);
    }
    pub fn phyAutoNegWait(_: C, port: u8, addr: u8, ms: u32, out: *anyopaque) u16 {
        return ra8_rmac_phy_auto_neg_wait(port, addr, ms, out);
    }
};

fn portOk(p: u8, msg: [*:0]const u8) bool {
    if (p < port_bases.len) return true;
    common.ra8_log_emit_error(tag, msg);
    return false;
}

fn nullPtr(msg: [*:0]const u8) u16 {
    common.ra8_log_emit_error(tag, msg);
    return common.k_ra8_err_null_ptr;
}

export fn ra8_etha_descriptor_ring_init(p: u8, tx: u16, rx: u16, buf: u16) u16 {
    if (!portOk(p, "etha_descriptor_ring_init: channel out of range")) return st.invalid_arg;
    return st.ringInit(&s_etha_slots[p].stats, Mmio{ .base = port_bases[p] }, C{}, tx, rx, buf);
}

export fn ra8_etha_descriptor_ring_init_cfg(p: u8, cfg: ?*const RingCfg) u16 {
    const c = cfg orelse return nullPtr("etha_descriptor_ring_init_cfg: cfg null");
    return ra8_etha_descriptor_ring_init(p, c.num_tx, c.num_rx, c.buffer_size);
}

export fn ra8_etha_get_stats(p: u8, out: ?*st.Stats) u16 {
    const o = out orelse return nullPtr("etha_get_stats: out_stats null");
    if (!portOk(p, "etha_get_stats: channel out of range")) return st.invalid_arg;
    o.* = s_etha_slots[p].stats;
    return common.k_ra8_ok;
}

export fn ra8_etha_account_traffic(p: u8, tx_ok: u32, tx_err: u32, rx_ok: u32, rx_err: u32, rx_drop: u32) u16 {
    if (!portOk(p, "etha_account_traffic: channel out of range")) return st.invalid_arg;
    st.account(&s_etha_slots[p].stats, tx_ok, tx_err, rx_ok, rx_err, rx_drop);
    return common.k_ra8_ok;
}

export fn ra8_etha_open(p: u8, phy: ?*const st.PhyOpen, out_link: ?*anyopaque) u16 {
    const ph = phy orelse return nullPtr("etha_open: phy null");
    const out = out_link orelse return nullPtr("etha_open: out_link null");
    if (!portOk(p, "etha_open: channel out of range")) return st.invalid_arg;
    return st.open(Mmio{ .base = port_bases[p] }, C{}, p, ph, out);
}
