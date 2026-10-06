//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for ra8_eth_gwca_default_recv (RA8FW-857). Logic lives in
//! internal/eth_gwca_recv.zig; it calls the ra8_eth_gwca_rx_frame and
//! ra8_eth_gwca_reload_queue exports.

const common = @import("abi_common.zig");
const r = @import("internal/eth_gwca_recv.zig");
const q = r.q;

const tag = "ETHGWC";

extern fn ra8_eth_gwca_rx_frame(chain: ?[*]volatile q.Desc, ring_depth: u32, head_idx: ?*u32, out_frame: ?[*]u8, out_capacity: u32, slot_bytes: u32, out_len: ?*u32) u16;
extern fn ra8_eth_gwca_reload_queue(queue_index: u32) u16;

const Hw = struct {
    pub fn rxFrame(_: Hw, chain: ?[*]volatile q.Desc, depth: u32, head: *u32, out: ?[*]u8, cap: u32, slot_bytes: u32, out_len: ?*u32) u16 {
        return ra8_eth_gwca_rx_frame(chain, depth, head, out, cap, slot_bytes, out_len);
    }
    pub fn reloadQueue(_: Hw, qi: u32) u16 {
        return ra8_eth_gwca_reload_queue(qi);
    }
    pub fn nullPtr(_: Hw, msg: [*:0]const u8) u16 {
        common.ra8_log_emit_error(tag, msg);
        return q.null_ptr;
    }
};

export fn ra8_eth_gwca_default_recv(state: ?*r.DefaultState, out_frame: ?[*]u8, out_capacity: u32, out_len: ?*u32) u16 {
    return r.recv(Hw{}, state, out_frame, out_capacity, out_len);
}
