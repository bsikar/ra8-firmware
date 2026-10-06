//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for ra8_eth_gwca_rx_frame (RA8FW-856). Logic lives in
//! internal/eth_gwca_rx.zig; the slot search is the existing
//! ra8_eth_gwca_find_slot export (src/eth_gwca_queue_abi.zig).

const common = @import("abi_common.zig");
const q = @import("internal/eth_gwca_queue.zig");
const rx = @import("internal/eth_gwca_rx.zig");

const tag = "ETHGWC";

extern fn ra8_eth_gwca_find_slot(chain: ?[*]const volatile q.Desc, ring_depth: u32, match_dt: u8, start_idx: u32, out_index: ?*u32) u16;

const Hw = struct {
    pub fn findSlot(_: Hw, chain: [*]volatile q.Desc, depth: u32, dt: u8, start: u32, out: *u32) u16 {
        return ra8_eth_gwca_find_slot(chain, depth, dt, start, out);
    }
    pub fn nullPtr(_: Hw, msg: [*:0]const u8) u16 {
        common.ra8_log_emit_error(tag, msg);
        return q.null_ptr;
    }
};

export fn ra8_eth_gwca_rx_frame(
    chain: ?[*]volatile q.Desc,
    ring_depth: u32,
    head_idx: ?*u32,
    out_frame: ?[*]u8,
    out_capacity: u32,
    slot_bytes: u32,
    out_len: ?*u32,
) u16 {
    return rx.rxFrame(Hw{}, chain, ring_depth, head_idx, out_frame, out_capacity, slot_bytes, out_len);
}
