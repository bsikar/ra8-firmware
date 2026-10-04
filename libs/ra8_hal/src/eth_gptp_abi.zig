//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the ra8_eth_gptp_* driver (RA8FW-589). The configured flag
//! lives here; the sequences are in internal/eth_gptp.zig.

const common = @import("abi_common.zig");
const gptp = @import("internal/eth_gptp.zig");

const tag = "ETHGPT";

extern fn ra8_mstp_enable(id: u16) u16;
extern fn ra8_mstp_disable(id: u16) u16;

/// `ra8_eth_gptp_cfg_t`.
const Cfg = extern struct { clk_hz: u32 };

var state: gptp.State = .{};

const Mmio = struct {
    pub fn read32(_: Mmio, off: usize) u32 {
        const p: *volatile u32 = @ptrFromInt(gptp.base_addr + off);
        return p.*;
    }
    pub fn write32(_: Mmio, off: usize, v: u32) void {
        const p: *volatile u32 = @ptrFromInt(gptp.base_addr + off);
        p.* = v;
    }
};

const C = struct {
    pub fn mstpEnable(_: C, id: u16) u16 {
        return ra8_mstp_enable(id);
    }
    pub fn mstpDisable(_: C, id: u16) u16 {
        return ra8_mstp_disable(id);
    }
    pub fn logError(_: C, msg: [*:0]const u8) void {
        common.ra8_log_emit_error(tag, msg);
    }
    pub fn logInfo(_: C, msg: [*:0]const u8) void {
        common.ra8_log_emit_info(tag, msg);
    }
    pub fn fail(_: C, msg: [*:0]const u8, err: u16) void {
        common.ra8_log_emit_error(tag, msg);
        common.ra8_log_emit_error_val(tag, "Error", err);
    }
};

fn nullPtr(msg: [*:0]const u8) u16 {
    common.ra8_log_emit_error(tag, msg);
    return common.k_ra8_err_null_ptr;
}

export fn ra8_eth_gptp_tiv_from_hz(clk_hz: u32, out: ?*u32) u16 {
    const o = out orelse return nullPtr("out_tiv must not be nullptr");
    o.* = gptp.tivFromHz(clk_hz) catch |e| return gptp.tivCode(e);
    return common.k_ra8_ok;
}

export fn ra8_eth_gptp_init(cfg: ?*const Cfg) u16 {
    const c = cfg orelse return nullPtr("cfg must not be nullptr");
    return state.init(Mmio{}, C{}, c.clk_hz);
}

export fn ra8_eth_gptp_deinit() u16 {
    return state.deinit(Mmio{}, C{});
}

export fn ra8_eth_gptp_ip_version(out: ?*u32) u16 {
    const o = out orelse return nullPtr("out_version must not be nullptr");
    return state.ipVersion(Mmio{}, C{}, o);
}

export fn ra8_eth_gptp_timer_enable(timer: u8) u16 {
    return state.enable(Mmio{}, C{}, timer);
}

export fn ra8_eth_gptp_timer_disable(timer: u8) u16 {
    return state.disable(Mmio{}, C{}, timer);
}

export fn ra8_eth_gptp_timer_is_enabled(timer: u8, out: ?*bool) u16 {
    const o = out orelse return nullPtr("out_enabled must not be nullptr");
    return state.isEnabled(Mmio{}, C{}, timer, o);
}

export fn ra8_eth_gptp_set_increment(timer: u8, tiv: u32) u16 {
    return state.setIncrement(Mmio{}, C{}, timer, tiv);
}

export fn ra8_eth_gptp_get_increment(timer: u8, out: ?*u32) u16 {
    const o = out orelse return nullPtr("out_tiv must not be nullptr");
    return state.getIncrement(Mmio{}, C{}, timer, o);
}

export fn ra8_eth_gptp_set_offset(timer: u8, sec: u64, nsec: u32) u16 {
    return state.setOffset(Mmio{}, C{}, timer, sec, nsec);
}

export fn ra8_eth_gptp_get_time(timer: u8, out_sec: ?*u64, out_nsec: ?*u32) u16 {
    const s = out_sec orelse return nullPtr("out_sec must not be nullptr");
    const n = out_nsec orelse return nullPtr("out_nsec must not be nullptr");
    var t: gptp.Time = undefined;
    const r = state.time(Mmio{}, C{}, timer, &t);
    if (r == common.k_ra8_ok) {
        s.* = t.sec;
        n.* = t.nsec;
    }
    return r;
}

export fn ra8_eth_gptp_get_avtp_ns(timer: u8, out: ?*u64) u16 {
    const o = out orelse return nullPtr("out_ns must not be nullptr");
    return state.avtpNs(Mmio{}, C{}, timer, o);
}

export fn ra8_eth_gptp_enter_stop() u16 {
    return state.enterStop(Mmio{}, C{});
}

export fn ra8_eth_gptp_exit_stop() u16 {
    return state.exitStop(C{});
}
