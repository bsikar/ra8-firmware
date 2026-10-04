//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for ra8_eth_link_status (RA8FW-617). ra8_eth.c keeps s_eth_state
//! and g_eth_mac_speed_resynced; the five debug globals live here.

const common = @import("abi_common.zig");
const el = @import("internal/eth_link.zig");

const tag = "ETH";

/// Leading part of ra8_eth_state_t: opened, then ra8_eth_cfg_t (channel
/// at offset 8). The stats tail is never touched from Zig.
const EthCfgHead = extern struct { mac_address: [6]u8, channel: u8 };
const EthStateHead = extern struct { opened: u8, cfg: EthCfgHead };

extern var s_eth_state: EthStateHead;
extern var g_eth_mac_speed_resynced: bool;

extern fn ra8_rmac_mdio_c22_read(port: u8, phy_addr: u8, reg: u8, out: *u16) u16;
extern fn ra8_rmac_set_link(port: u8, pis: u8, speed: u8, duplex: u8) u16;
extern fn ra8_etha_set_mode(port: u8, mode: u8) u16;
extern fn ra8_delay_ms(ms: u32) void;

export var g_ra8_eth_phy_bmsr_after_wait: u16 = 0;
export var g_ra8_eth_anlpar: u16 = 0;
export var g_ra8_eth_gbsr: u16 = 0;
export var g_ra8_eth_resync_speed_lsc: u32 = 0;
export var g_ra8_eth_resync_duplex: u32 = 0;

fn put(comptime T: type, p: *T, v: T) void {
    @as(*volatile T, p).* = v;
}

const Phy = struct {
    pub fn mdioRead(_: Phy, port: u8, reg: u8, out: *u16) u16 {
        return ra8_rmac_mdio_c22_read(port, el.phy_addr, reg, out);
    }
    pub fn setLink(_: Phy, port: u8, pis: u8, speed: u8, duplex: u8) u16 {
        return ra8_rmac_set_link(port, pis, speed, duplex);
    }
    pub fn ethaSetMode(_: Phy, port: u8, mode: u8) u16 {
        return ra8_etha_set_mode(port, mode);
    }
    pub fn delayMs(_: Phy, ms: u32) void {
        ra8_delay_ms(ms);
    }
    pub fn traceBmsr(_: Phy, bmsr: u16) void {
        put(u16, &g_ra8_eth_phy_bmsr_after_wait, bmsr);
    }
    pub fn traceAdvert(_: Phy, anlpar: u16, gbsr: u16) void {
        put(u16, &g_ra8_eth_anlpar, anlpar);
        put(u16, &g_ra8_eth_gbsr, gbsr);
    }
    pub fn traceResync(_: Phy, speed: u8, duplex: u8) void {
        put(u32, &g_ra8_eth_resync_speed_lsc, speed);
        put(u32, &g_ra8_eth_resync_duplex, duplex);
    }
    pub fn info(_: Phy, msg: [*:0]const u8) void {
        common.ra8_log_emit_info(tag, msg);
    }
    pub fn err(_: Phy, msg: [*:0]const u8) void {
        common.ra8_log_emit_error(tag, msg);
    }
    pub fn errVal(_: Phy, msg: [*:0]const u8, value: u32) void {
        common.ra8_log_emit_error_val(tag, msg, value);
    }
};

export fn ra8_eth_link_status(out_status: ?*el.Link) u16 {
    const opened = s_eth_state.opened != 0;
    return el.linkStatus(Phy{}, out_status, opened, s_eth_state.cfg.channel, &g_eth_mac_speed_resynced);
}
