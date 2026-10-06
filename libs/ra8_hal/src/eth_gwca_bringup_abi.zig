//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for GWCA LINKFIX install and bring-up (RA8FW-850). The J-Link step
//! globals are defined here under their C names; eth_gwca_open_abi.zig
//! writes open/pre_step through extern vars.

const common = @import("abi_common.zig");
const q = @import("internal/eth_gwca_queue.zig");
const b = @import("internal/eth_gwca_bringup.zig");

const tag = "ETHGWC";
/// `k_ra8_gwca0_base_addr` and GWDCBAC0/1 (ra8_ether_regs.h).
const gwca0_base: usize = 0x403CE000;
const off_gwdcbac0: usize = 0x0194;
const off_gwdcbac1: usize = 0x0198;

extern fn ra8_eth_gwca_set_operation_mode(mode: u32) u16;
extern fn ra8_eth_gwca_axi_init() u16;

/// Read by J-Link only; firmware never reads them back.
export var g_ra8_eth_gwca_open_step: u32 = 0;
export var g_ra8_eth_gwca_pre_step: u32 = 0;
export var g_ra8_eth_gwca_bring_up_step: u32 = 0;

fn gwcaReg(off: usize) *volatile u32 {
    return @ptrFromInt(gwca0_base + off);
}

const Log = struct {
    pub fn logError(_: Log, msg: [*:0]const u8) void {
        common.ra8_log_emit_error(tag, msg);
    }
};

const Ops = struct {
    pub fn setMode(_: Ops, mode: u32) u16 {
        return ra8_eth_gwca_set_operation_mode(mode);
    }
    pub fn axiInit(_: Ops) u16 {
        return ra8_eth_gwca_axi_init();
    }
    pub fn installLinkfix(_: Ops, table: ?[*]volatile q.Desc, count: u32) u16 {
        return ra8_eth_gwca_install_linkfix(table, count);
    }
    pub fn step(_: Ops, value: u32) void {
        const p: *volatile u32 = &g_ra8_eth_gwca_bring_up_step;
        p.* = value;
    }
};

export fn ra8_eth_gwca_install_linkfix(table: ?[*]volatile q.Desc, entry_count: u32) u16 {
    return b.installLinkfix(Log{}, table, entry_count, gwcaReg(off_gwdcbac0), gwcaReg(off_gwdcbac1));
}

export fn ra8_eth_gwca_bring_up(table: ?[*]volatile q.Desc, entry_count: u32) u16 {
    return b.bringUp(Ops{}, table, entry_count);
}
