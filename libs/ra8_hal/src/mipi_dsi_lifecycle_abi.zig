//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for ra8_mipi_dsi_init / deinit / enter_stop / exit_stop
//! (RA8FW-645). With this the last of ra8_mipi_dsi.c is in Zig and the C
//! file is gone. The event callback and pending-RX globals are defined
//! here and declared extern in ra8_mipi_dsi_internal.h for the dispatcher.

const common = @import("abi_common.zig");
const lc = @import("internal/mipi_dsi_lifecycle.zig");

const tag = "MIPI_DSI";
const base_addr: usize = 0x40346000;
/// `ra8_mstp_t` k_ra8_mstp_mipi_dsi: (k_ra8_mstp_reg_c << 8) | 10 (inc/ra8_mstp_regs.h).
const mstp_mipi_dsi: u16 = (2 << 8) | 10;

export var s_mipi_dsi_event_fn: ?*const anyopaque = null;
export var s_mipi_dsi_event_ctx: ?*anyopaque = null;
export var s_mipi_dsi_pending_rx_buffer: ?[*]u8 = null;
export var s_mipi_dsi_pending_rx_len: u16 = 0;
var s_initialized: bool = false;

/// Defined in mipi_dsi_lanes_abi.zig (RA8FW-643).
extern var s_mipi_dsi_continuous_clock: bool;
extern var s_mipi_dsi_clock_lanes_in_ulps: bool;
extern var s_mipi_dsi_data_lanes_in_ulps: bool;

extern fn ra8_mstp_enable(id: u16) u16;
extern fn ra8_mstp_disable(id: u16) u16;

const Dsi = struct {
    pub fn write32(_: Dsi, off: u16, value: u32) void {
        @as(*volatile u32, @ptrFromInt(base_addr + off)).* = value;
    }
    /// RA8_CHECK_NULL_PTR's single log line.
    pub fn err(_: Dsi, msg: [*:0]const u8) void {
        common.ra8_log_emit_error(tag, msg);
    }
    /// Same two log lines RA8_RETURN_ON_ERROR emits.
    pub fn errVal(_: Dsi, msg: [*:0]const u8, code: u16) void {
        common.ra8_log_emit_error(tag, msg);
        common.ra8_log_emit_error_val(tag, "Error", code);
    }
    pub fn info(_: Dsi, msg: [*:0]const u8) void {
        common.ra8_log_emit_info(tag, msg);
    }
    pub fn mstpEnable(_: Dsi) u16 {
        return ra8_mstp_enable(mstp_mipi_dsi);
    }
    pub fn mstpDisable(_: Dsi) u16 {
        return ra8_mstp_disable(mstp_mipi_dsi);
    }
};

fn state() lc.State {
    return .{
        .initialized = &s_initialized,
        .continuous_clock = &s_mipi_dsi_continuous_clock,
        .clock_ulps = &s_mipi_dsi_clock_lanes_in_ulps,
        .data_ulps = &s_mipi_dsi_data_lanes_in_ulps,
        .event_fn = &s_mipi_dsi_event_fn,
        .event_ctx = &s_mipi_dsi_event_ctx,
        .rx_buffer = &s_mipi_dsi_pending_rx_buffer,
        .rx_len = &s_mipi_dsi_pending_rx_len,
    };
}

export fn ra8_mipi_dsi_init(cfg: ?*const lc.Config) u16 {
    return lc.init(Dsi{}, state(), cfg);
}

export fn ra8_mipi_dsi_deinit() u16 {
    return lc.deinit(Dsi{}, state());
}

export fn ra8_mipi_dsi_enter_stop() u16 {
    return lc.enterStop(Dsi{}, state());
}

export fn ra8_mipi_dsi_exit_stop() u16 {
    return lc.exitStop(Dsi{}, state());
}
