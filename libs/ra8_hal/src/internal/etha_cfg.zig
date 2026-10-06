//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! ETHA queue and VLAN configuration (RA8FW-814, was part of ra8_etha.c).
//! Pure: one port's ETHA block comes in as a `regs` value (read32/write32 by
//! offset). Port and pointer checks stay in src/etha_cfg_abi.zig.

pub const tc_count = 8;

pub const off_eairc: usize = 0x010;
pub const off_eatdqac: usize = 0x01C;
pub const off_eatpec: usize = 0x020;
pub const off_eatmfsc: usize = 0x040; // EATMFSCq, 8 x u32
pub const off_eatdqdc: usize = 0x060; // EATDQDCq, 8 x u32
pub const off_eatdqm: usize = 0x080; // EATDQMq, 8 x u32
pub const off_eatdqmlm: usize = 0x0A0; // EATDQMLMq, 8 x u32
pub const off_eactqc: usize = 0x100;
pub const off_eactdqdc: usize = 0x104;
pub const off_eavcc: usize = 0x130;
pub const off_eavtc: usize = 0x134;
pub const off_eartfc: usize = 0x138;

pub const mask_tdqa: u32 = 0xF;
pub const mask_dqd: u32 = 0x7FF;
pub const mask_dnq: u32 = 0x7FF;
pub const mask_mfs: u32 = 0xFFFF;
pub const mask_ipv: u32 = 0x7;
pub const mask_ctqd: u32 = 0xFFFF;
pub const mask_ctdqd: u32 = 0xF;
pub const mask_vid: u32 = 0xFFF;
pub const mask_pcp: u32 = 0x7;
pub const mask_dei: u32 = 0x1;
pub const mask_vem: u32 = 0x7;
pub const mask_rx_tag: u32 = 0x1FF;

/// `ra8_etha_vlan_tag_t`.
pub const VlanTag = extern struct { vid: u16, pcp: u8, dei: u8 };

pub fn tcOk(tc: u8) bool {
    return tc < tc_count;
}

/// EATDQAC.TDQAq: 4 bits per class, read-modify-write (HUM 32.3.2.4 p 1634).
pub fn setQueueArb(regs: anytype, tc: u8, arb: u8) void {
    const shift: u5 = @intCast(@as(u32, tc) * 4);
    const mask = mask_tdqa << shift;
    const v = regs.read32(off_eatdqac);
    regs.write32(off_eatdqac, (v & ~mask) | ((@as(u32, arb) & mask_tdqa) << shift));
}

/// EATDQMq / EATDQMLMq current and peak level (HUM 32.3.2.8-9 p 1636-1637).
pub fn queueLevel(regs: anytype, tc: u8) [2]u16 {
    const at = 4 * @as(usize, tc);
    return .{
        @intCast(regs.read32(off_eatdqm + at) & mask_dnq),
        @intCast(regs.read32(off_eatdqmlm + at) & mask_dnq),
    };
}

/// EATPEC: preempt byte, TTQ8 cut-through at bit 8, AFS at 17:16 (HUM 32.3.2.5 p 1635).
pub fn preemption(preempt: u8, cut_thru: u8, afs: u8) u32 {
    var v: u32 = preempt;
    if (cut_thru != 0) v |= 1 << 8;
    return v | ((@as(u32, afs) & 0x3) << 16);
}

/// True when every IPV remap entry fits in three bits.
pub fn ipvMapOk(map: *const [tc_count]u8) bool {
    for (map) |e| if (e > mask_ipv) return false;
    return true;
}

/// EAIRC: one nibble per class, entry q at bits 4q+2..4q (HUM 32.3.2.1 p 1631).
pub fn ipvPack(map: *const [tc_count]u8) u32 {
    var packed_map: u32 = 0;
    for (map, 0..) |e, i| packed_map |= (@as(u32, e) & mask_ipv) << @intCast(4 * i);
    return packed_map;
}

/// EAVCC: VIM at bit 0, VEM at 18:16 (HUM 32.3.3.1 p 1639).
pub fn vlanMode(vim: u8, vem: u8) u32 {
    return (@as(u32, vim) & 0x1) | ((@as(u32, vem) & mask_vem) << 16);
}

pub fn tagOk(t: *const VlanTag) bool {
    return t.vid <= mask_vid and t.pcp <= mask_pcp and t.dei <= mask_dei;
}

/// EAVTC: C-tag in 15:0 (VID 11:0, PCP 14:12, DEI 15), S-tag in 31:16 (HUM 32.3.3.2 p 1640).
pub fn vlanTag(c: *const VlanTag, s: *const VlanTag) u32 {
    return half(c) | (half(s) << 16);
}

fn half(t: *const VlanTag) u32 {
    return (@as(u32, t.vid) & mask_vid) | ((@as(u32, t.pcp) & mask_pcp) << 12) | ((@as(u32, t.dei) & mask_dei) << 15);
}
