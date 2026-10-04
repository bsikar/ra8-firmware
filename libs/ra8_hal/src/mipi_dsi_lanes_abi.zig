//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for ra8_mipi_dsi_soft_reset, hs_clock_start/stop, ulps_enter/exit
//! and priv_ra8_mipi_dsi_internal_wait_eq (RA8FW-643), replacing that part
//! of ra8_mipi_dsi.c. The three lane-state flags are defined here and
//! declared extern in ra8_mipi_dsi_internal.h; init/deinit in
//! mipi_dsi_lifecycle_abi.zig (RA8FW-645) set them.

const common = @import("abi_common.zig");
const ln = @import("internal/mipi_dsi_lanes.zig");

const tag = "MIPI_DSI";
const base_addr: usize = 0x40346000;

export var s_mipi_dsi_continuous_clock: bool = false;
export var s_mipi_dsi_clock_lanes_in_ulps: bool = false;
export var s_mipi_dsi_data_lanes_in_ulps: bool = false;

const Dsi = struct {
    pub fn read32(_: Dsi, off: u16) u32 {
        return @as(*volatile u32, @ptrFromInt(base_addr + off)).*;
    }
    pub fn write32(_: Dsi, off: u16, value: u32) void {
        @as(*volatile u32, @ptrFromInt(base_addr + off)).* = value;
    }
    pub fn errMsg(_: Dsi, msg: [*:0]const u8) void {
        common.ra8_log_emit_error(tag, msg);
    }
};

const PtrReader = struct {
    reg: *const volatile u32,
    pub fn read(r: PtrReader) u32 {
        return r.reg.*;
    }
};

fn flags() ln.Flags {
    return .{
        .continuous_clock = &s_mipi_dsi_continuous_clock,
        .clock_ulps = &s_mipi_dsi_clock_lanes_in_ulps,
        .data_ulps = &s_mipi_dsi_data_lanes_in_ulps,
    };
}

export fn priv_ra8_mipi_dsi_internal_wait_eq(reg: *const volatile u32, mask: u32, expect: u32) u16 {
    return ln.waitEq(PtrReader{ .reg = reg }, mask, expect);
}

export fn ra8_mipi_dsi_soft_reset() u16 {
    return ln.softReset(Dsi{});
}

export fn ra8_mipi_dsi_hs_clock_start() u16 {
    return ln.hsClockStart(Dsi{}, flags());
}

export fn ra8_mipi_dsi_hs_clock_stop() u16 {
    return ln.hsClockStop(Dsi{});
}

export fn ra8_mipi_dsi_ulps_enter(lanes: u8) u16 {
    return ln.ulpsEnter(Dsi{}, flags(), lanes);
}

export fn ra8_mipi_dsi_ulps_exit(lanes: u8) u16 {
    return ln.ulpsExit(Dsi{}, flags(), lanes);
}
