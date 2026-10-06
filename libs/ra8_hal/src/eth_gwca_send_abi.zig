//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for ra8_eth_gwca_default_send (RA8FW-858). Logic lives in
//! internal/eth_gwca_send.zig; it calls the ra8_eth_gwca_reload_queue and
//! ra8_eth_gwca_kick_tx exports.

const builtin = @import("builtin");
const common = @import("abi_common.zig");
const t = @import("internal/eth_gwca_send.zig");
const q = t.q;

const tag = "ETHGWC";

extern fn ra8_eth_gwca_reload_queue(queue_index: u32) u16;
extern fn ra8_eth_gwca_kick_tx(queue_index: u32) u16;
extern fn ra8_hw_dsb() void;

/// Host builds go through the C fake-MMIO wait seam, keyed on the descriptor
/// base, so the C suites can drive the write-back to done or to timeout.
const hosted = builtin.os.tag != .freestanding;
const seam = struct {
    extern fn ra8_fake_mmio_wait_eval(reg: *const volatile anyopaque, iter: u32, real_cond: bool) bool;
};

const Hw = struct {
    pub fn dsb(_: Hw) void {
        ra8_hw_dsb();
    }
    pub fn reloadQueue(_: Hw, qi: u32) u16 {
        return ra8_eth_gwca_reload_queue(qi);
    }
    pub fn kickTx(_: Hw, qi: u32) u16 {
        return ra8_eth_gwca_kick_tx(qi);
    }
    pub fn txDone(_: Hw, d: *volatile t.ExtDesc, iter: u32) bool {
        const cond = q.getDt(&d.base) != t.dt_fsingle;
        return if (hosted) seam.ra8_fake_mmio_wait_eval(d, iter, cond) else cond;
    }
    pub fn nullPtr(_: Hw, msg: [*:0]const u8) u16 {
        common.ra8_log_emit_error(tag, msg);
        return q.null_ptr;
    }
    pub fn logError(_: Hw, msg: [*:0]const u8) void {
        common.ra8_log_emit_error(tag, msg);
    }
};

export fn ra8_eth_gwca_default_send(state: ?*t.DefaultState, frame: ?[*]const u8, len: u32) u16 {
    return t.send(Hw{}, state, frame, len);
}
