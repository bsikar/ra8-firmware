//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for GWCA status, handler attach and dispatch (RA8FW-847). s_gwca_fn
//! and s_gwca_ctx are defined here under their C names; eth_gwca_life_abi.zig
//! clears them in init and deinit through extern declarations.

const common = @import("abi_common.zig");
const ev = @import("internal/eth_gwca_events.zig");

const tag = "ETHGWC";
/// `k_ra8_gwca0_base_addr` (ra8_ether_regs.h).
const gwca0_base: usize = 0x403CE000;

extern fn ra8_log_error(tag: [*:0]const u8, msg: [*:0]const u8) void;

/// Active event callback; null means none.
export var s_gwca_fn: ?ev.EventFn = null;
/// Context handed to s_gwca_fn.
export var s_gwca_ctx: ?*anyopaque = null;

fn regs() *volatile ev.Regs {
    return @ptrFromInt(gwca0_base);
}

export fn ra8_eth_gwca_get_status(out_mask: ?*u32) u16 {
    const out = out_mask orelse {
        ra8_log_error(tag, "out_mask must not be nullptr");
        return common.k_ra8_err_null_ptr;
    };
    out.* = regs().sts;
    return common.k_ra8_ok;
}

export fn ra8_eth_gwca_clear_status(mask: u32) u16 {
    ev.clearStatus(regs(), mask);
    return common.k_ra8_ok;
}

export fn ra8_eth_gwca_attach_handler(f: ?ev.EventFn, ctx: ?*anyopaque) u16 {
    s_gwca_fn = f;
    s_gwca_ctx = ctx;
    return common.k_ra8_ok;
}

export fn ra8_eth_gwca_dispatch() void {
    ev.dispatch(regs(), s_gwca_fn, s_gwca_ctx);
}
