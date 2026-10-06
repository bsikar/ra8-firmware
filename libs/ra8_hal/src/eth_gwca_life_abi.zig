//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for ra8_eth_gwca_init, deinit and enter_stop (RA8FW-851). Logic
//! lives in internal/eth_gwca_life.zig. s_gwca_fn/ctx are defined in
//! eth_gwca_events_abi.zig and cleared here through extern declarations.

const common = @import("abi_common.zig");
const events = @import("internal/eth_gwca_events.zig");
const life = @import("internal/eth_gwca_life.zig");

const tag = "ETHGWC";
/// `k_ra8_gwca0_base_addr` and `k_ra8_mfwd_base_addr` (ra8_ether_regs.h).
const gwca0_base: usize = 0x403CE000;
const mfwd_base: usize = 0x403C0000;
/// FWPC10 (GWCA agent), FWPC11 (ETHA0), FWPC12 (ETHA1).
const off_fwpc = [3]usize{ 0x104, 0x114, 0x124 };
/// `ra8_mstp_t` k_ra8_mstp_eswm: (k_ra8_mstp_reg_c << 8) | 30.
const mstp_eswm: u16 = (2 << 8) | 30;

extern fn ra8_mstp_enable(id: u16) u16;
extern fn ra8_mstp_disable(id: u16) u16;
extern var s_gwca_fn: ?events.EventFn;
extern var s_gwca_ctx: ?*anyopaque;

const Ops = struct {
    pub fn mstpEnable(_: Ops) u16 {
        return ra8_mstp_enable(mstp_eswm);
    }
    pub fn mstpDisable(_: Ops) u16 {
        return ra8_mstp_disable(mstp_eswm);
    }
    pub fn fail(_: Ops, msg: [*:0]const u8, err: u16) void {
        common.ra8_log_emit_error(tag, msg);
        common.ra8_log_emit_error_val(tag, "Error", err);
    }
    pub fn info(_: Ops, msg: [*:0]const u8) void {
        common.ra8_log_emit_info(tag, msg);
    }
    pub fn clearHandler(_: Ops) void {
        s_gwca_fn = null;
        s_gwca_ctx = null;
    }
};

fn view() life.View {
    return .{
        .gwca = @ptrFromInt(gwca0_base),
        .fwpc = .{
            @ptrFromInt(mfwd_base + off_fwpc[0]),
            @ptrFromInt(mfwd_base + off_fwpc[1]),
            @ptrFromInt(mfwd_base + off_fwpc[2]),
        },
    };
}

export fn ra8_eth_gwca_init() u16 {
    return life.init(Ops{}, view());
}

export fn ra8_eth_gwca_deinit() u16 {
    return life.deinit(Ops{}, view());
}

export fn ra8_eth_gwca_enter_stop() u16 {
    return life.enterStop(Ops{}, view());
}
