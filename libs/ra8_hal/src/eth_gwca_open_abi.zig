//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for ra8_eth_gwca_default_open (RA8FW-859). Logic lives in
//! internal/eth_gwca_open.zig; it calls the GWCA exports below and writes
//! the J-Link step globals defined in eth_gwca_bringup_abi.zig.

const common = @import("abi_common.zig");
const o = @import("internal/eth_gwca_open.zig");
const q = o.q;

const tag = "ETHGWC";

extern fn ra8_eth_gwca_init() u16;
extern fn ra8_eth_gwca_init_ring(chain: ?[*]volatile q.Desc, ring_depth: u32, slot_bytes: u32) u16;
extern fn ra8_eth_gwca_attach_buffers(chain: ?[*]volatile q.Desc, ring_depth: u32, slot_bytes: u32, pool: ?[*]u8) u16;
extern fn ra8_eth_gwca_bring_up(table: ?[*]volatile q.Desc, entry_count: u32) u16;
extern fn ra8_eth_gwca_set_operation_mode(mode: u32) u16;
extern fn ra8_eth_gwca_configure_queue(table: ?[*]volatile q.Desc, queue: u32, cfg: ?*const q.QueueCfg) u16;
extern fn ra8_eth_gwca_reload_queue(queue_index: u32) u16;
extern var g_ra8_eth_gwca_open_step: u32;
extern var g_ra8_eth_gwca_pre_step: u32;

const Hw = struct {
    pub fn init(_: Hw) u16 {
        return ra8_eth_gwca_init();
    }
    pub fn initRing(_: Hw, chain: ?[*]volatile q.Desc, depth: u32, slot: u32) u16 {
        return ra8_eth_gwca_init_ring(chain, depth, slot);
    }
    pub fn attachBuffers(_: Hw, chain: ?[*]volatile q.Desc, depth: u32, slot: u32, pool: ?[*]u8) u16 {
        return ra8_eth_gwca_attach_buffers(chain, depth, slot, pool);
    }
    pub fn bringUp(_: Hw, table: ?[*]volatile q.Desc, count: u32) u16 {
        return ra8_eth_gwca_bring_up(table, count);
    }
    pub fn setMode(_: Hw, opc: u32) u16 {
        return ra8_eth_gwca_set_operation_mode(opc);
    }
    pub fn configureQueue(_: Hw, table: ?[*]volatile q.Desc, qi: u32, cfg: *const q.QueueCfg) u16 {
        return ra8_eth_gwca_configure_queue(table, qi, cfg);
    }
    pub fn reloadQueue(_: Hw, qi: u32) u16 {
        return ra8_eth_gwca_reload_queue(qi);
    }
    pub fn setPreStep(_: Hw, v: u32) void {
        const p: *volatile u32 = &g_ra8_eth_gwca_pre_step;
        p.* = v;
    }
    pub fn setOpenStep(_: Hw, v: u32) void {
        const p: *volatile u32 = &g_ra8_eth_gwca_open_step;
        p.* = v;
    }
    pub fn nullPtr(_: Hw, msg: [*:0]const u8) u16 {
        common.ra8_log_emit_error(tag, msg);
        return q.null_ptr;
    }
    pub fn fail(_: Hw, msg: [*:0]const u8, code: u16) u16 {
        common.ra8_log_emit_error(tag, msg);
        common.ra8_log_emit_error_val(tag, "Error", code);
        return code;
    }
};

export fn ra8_eth_gwca_default_open(state: ?*o.DefaultState) u16 {
    return o.open(Hw{}, state);
}
