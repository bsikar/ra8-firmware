//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the ra8_lvd.h event functions (RA8FW-625), replacing
//! ra8_lvd_events.c. The channel map and CR0 helpers are in lvd_abi.zig.

const common = @import("abi_common.zig");
const ev = @import("internal/lvd_events.zig");

const tag = "LVD";

extern const g_lvd_map: [ev.map_count]ev.Map;
extern fn priv_ra8_lvd_internal_channel_to_idx(channel: u8, out_idx: *u8) u16;
extern fn priv_ra8_lvd_internal_cr0_rmw(map: *const ev.Map, clear_mask: u8, set_bits: u8) void;

const Lvd = struct {
    pub fn read8(_: Lvd, addr: usize) u8 {
        return @as(*volatile u8, @ptrFromInt(addr)).*;
    }
    pub fn write8(_: Lvd, addr: usize, value: u8) void {
        @as(*volatile u8, @ptrFromInt(addr)).* = value;
    }
    pub fn write32(_: Lvd, addr: usize, value: u32) void {
        @as(*volatile u32, @ptrFromInt(addr)).* = value;
    }
    pub fn channelToIdx(_: Lvd, channel: u8, idx: *u8) u16 {
        return priv_ra8_lvd_internal_channel_to_idx(channel, idx);
    }
    pub fn map(_: Lvd, idx: u8) ev.Map {
        return g_lvd_map[idx];
    }
    pub fn cr0Rmw(_: Lvd, m: *const ev.Map, clear_mask: u8, set_bits: u8) void {
        priv_ra8_lvd_internal_cr0_rmw(m, clear_mask, set_bits);
    }
    pub fn errVal(_: Lvd, msg: [*:0]const u8, value: u16) void {
        common.ra8_log_emit_error_val(tag, msg, value);
    }
};

var state: ev.State = .{};

export fn ra8_lvd_set_security(mask: u32) u16 {
    return ev.setSecurity(Lvd{}, mask);
}

export fn ra8_lvd_unlock_n_channels() u16 {
    return ev.unlockN(Lvd{});
}

export fn ra8_lvd_relock_n_channels() u16 {
    return ev.relockN(Lvd{});
}

export fn ra8_lvd_enable_elc_event(channel: u8) u16 {
    return ev.enableElcEvent(Lvd{}, channel);
}

export fn ra8_lvd_disable_elc_event(channel: u8) u16 {
    return ev.disableElcEvent(Lvd{}, channel);
}

export fn ra8_lvd_configure_for_standby(channel: u8) u16 {
    return ev.configureForStandby(Lvd{}, channel);
}

export fn ra8_lvd_cancel_deep_standby_path() u16 {
    return ev.cancelDeepStandbyPath(Lvd{});
}

export fn ra8_lvd_filter_delay_us(div: u8, loco_hz: u32) u32 {
    return ev.filterDelayUs(div, loco_hz);
}

export fn ra8_lvd_attach_handler(f: ev.EventFn, ctx: ?*anyopaque) u16 {
    return ev.attachHandler(&state, f, ctx);
}

export fn ra8_lvd_attach_channel_handler(channel: u8, f: ev.EventFn, ctx: ?*anyopaque) u16 {
    return ev.attachChannelHandler(&state, Lvd{}, channel, f, ctx);
}

export fn ra8_lvd_dispatch(channel: u8) void {
    ev.dispatch(&state, Lvd{}, channel);
}
