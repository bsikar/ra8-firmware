//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the ra8_lvd.h runtime controls (RA8FW-739), replacing
//! ra8_lvd_runtime.c. The channel map and helpers stay in ra8_lvd.c.

const common = @import("abi_common.zig");
const rt = @import("internal/lvd_runtime.zig");

const tag = "LVD";

extern const g_lvd_map: [4]rt.Map;
extern fn priv_ra8_lvd_internal_channel_to_idx(channel: u8, out_idx: *u8) u16;
extern fn priv_ra8_lvd_internal_cr0_rmw(map: *const rt.Map, clear_mask: u8, set_bits: u8) void;
extern fn priv_ra8_lvd_internal_validate_div(div: u8) u16;
extern fn priv_ra8_lvd_internal_read_ri(map: *const rt.Map) u8;

const Lvd = struct {
    pub fn read8(_: Lvd, addr: usize) u8 {
        return @as(*volatile u8, @ptrFromInt(addr)).*;
    }
    pub fn write8(_: Lvd, addr: usize, value: u8) void {
        @as(*volatile u8, @ptrFromInt(addr)).* = value;
    }
    pub fn channelToIdx(_: Lvd, channel: u8, idx: *u8) u16 {
        return priv_ra8_lvd_internal_channel_to_idx(channel, idx);
    }
    pub fn map(_: Lvd, idx: u8) rt.Map {
        return g_lvd_map[idx];
    }
    pub fn cr0Rmw(_: Lvd, m: *const rt.Map, clear_mask: u8, set_bits: u8) void {
        priv_ra8_lvd_internal_cr0_rmw(m, clear_mask, set_bits);
    }
    pub fn validateDiv(_: Lvd, div: u8) u16 {
        return priv_ra8_lvd_internal_validate_div(div);
    }
    pub fn readRi(_: Lvd, m: *const rt.Map) u8 {
        return priv_ra8_lvd_internal_read_ri(m);
    }
    pub fn errVal(_: Lvd, msg: [*:0]const u8, value: u16) void {
        common.ra8_log_emit_error_val(tag, msg, value);
    }
    pub fn err(_: Lvd, msg: [*:0]const u8) void {
        common.ra8_log_emit_error(tag, msg);
    }
};

export fn ra8_lvd_enable_irq(channel: u8) u16 {
    return rt.enableIrq(Lvd{}, channel);
}

export fn ra8_lvd_disable_irq(channel: u8) u16 {
    return rt.disableIrq(Lvd{}, channel);
}

export fn ra8_lvd_enable_reset(channel: u8) u16 {
    return rt.enableReset(Lvd{}, channel);
}

export fn ra8_lvd_disable_reset(channel: u8) u16 {
    return rt.disableReset(Lvd{}, channel);
}

export fn ra8_lvd_enable_cmpe(channel: u8) u16 {
    return rt.enableCmpe(Lvd{}, channel);
}

export fn ra8_lvd_disable_cmpe(channel: u8) u16 {
    return rt.disableCmpe(Lvd{}, channel);
}

export fn ra8_lvd_set_filter(channel: u8, filter_div: u8, filter_en: bool) u16 {
    return rt.setFilter(Lvd{}, channel, filter_div, filter_en);
}

export fn ra8_lvd_set_hysteresis_mode(channel: u8, hyst: u8) u16 {
    return rt.setHysteresisMode(Lvd{}, channel, hyst);
}

export fn ra8_lvd_set_negate_mode(channel: u8, negate: u8) u16 {
    return rt.setNegateMode(Lvd{}, channel, negate);
}

export fn ra8_lvd_get_status(channel: u8, out: ?*rt.Status) u16 {
    return rt.getStatus(Lvd{}, channel, out);
}

export fn ra8_lvd_clear_status(channel: u8) u16 {
    return rt.clearStatus(Lvd{}, channel);
}
