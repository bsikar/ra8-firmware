//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for ra8_canfd_init / ra8_canfd_deinit (RA8FW-864). Logic lives in
//! internal/canfd_init.zig; the mode handshakes and open_channel are the
//! priv_ exports in canfd_mode_abi.zig.

const builtin = @import("builtin");
const common = @import("abi_common.zig");
const ini = @import("internal/canfd_init.zig");
const tdc = @import("internal/canfd_tdc.zig");

const tag = "CANFD";
const sys_base: usize = 0x4001_E000;
const off_prcr: usize = 0x3FA;
const off_ckdivcr: usize = 0x06E;
const off_ckcr: usize = 0x076;
/// `k_ra8_chmdc_reset`.
const chmdc_reset: c_uint = 1;

extern fn ra8_mstp_enable(id: u16) u16;
extern fn priv_ra8_canfd_internal_open_channel(reg: *volatile anyopaque) u16;
extern fn priv_ra8_canfd_internal_set_channel_mode(reg: *volatile anyopaque, mode: c_uint) u16;

const hosted = builtin.os.tag != .freestanding;
const seam = struct {
    extern fn ra8_fake_mmio_wait_eval(reg: *const volatile anyopaque, iter: u32, real_cond: bool) bool;
};

/// Static C flag `s_canfd_clock_inited`: the clock block runs once per boot.
var clock_inited: bool = false;

fn sys8(off: usize) *volatile u8 {
    return @ptrFromInt(sys_base + off);
}

fn chan(channel: u8) *volatile anyopaque {
    return @ptrFromInt(tdc.channel_bases[channel]);
}

const Hw = struct {
    pub fn prcr(_: Hw, value: u16) void {
        @as(*volatile u16, @ptrFromInt(sys_base + off_prcr)).* = value;
    }
    pub fn writeDivcr(_: Hw, v: u8) void {
        sys8(off_ckdivcr).* = v;
    }
    pub fn writeCkcr(_: Hw, v: u8) void {
        sys8(off_ckcr).* = v;
    }
    pub fn waitSrdy(_: Hw, set: bool) bool {
        const reg = sys8(off_ckcr);
        var i: u32 = 0;
        while (i < ini.ckcr_spin) : (i += 1) {
            const cond = ((reg.* & ini.srdy) != 0) == set;
            if (if (hosted) seam.ra8_fake_mmio_wait_eval(reg, i, cond) else cond) return true;
        }
        return false;
    }
    pub fn mstpEnable(_: Hw, id: u16) u16 {
        return ra8_mstp_enable(id);
    }
    /// Bounded, silent: the C loop just stops polling on expiry.
    pub fn waitGramInit(_: Hw, channel: u8) void {
        const gsts: *volatile u32 = @ptrFromInt(tdc.channel_bases[channel] + ini.off_gsts);
        var i: u32 = 0;
        while (i < ini.graminit_spin) : (i += 1) {
            if (gsts.* & ini.graminit == 0) return;
        }
    }
    pub fn openChannel(_: Hw, channel: u8) u16 {
        return priv_ra8_canfd_internal_open_channel(chan(channel));
    }
    pub fn channelReset(_: Hw, channel: u8) u16 {
        return priv_ra8_canfd_internal_set_channel_mode(chan(channel), chmdc_reset);
    }
    pub fn info(_: Hw, msg: [*:0]const u8) void {
        common.ra8_log_emit_info(tag, msg);
    }
    pub fn infoVal(_: Hw, msg: [*:0]const u8, value: u32) void {
        common.ra8_log_emit_info_val(tag, msg, value);
    }
    pub fn err(_: Hw, msg: [*:0]const u8) void {
        common.ra8_log_emit_error(tag, msg);
    }
    pub fn fail(_: Hw, msg: [*:0]const u8, code: u16) void {
        common.ra8_log_emit_error(tag, msg);
        common.ra8_log_emit_error_val(tag, "Error", code);
    }
};

export fn ra8_canfd_init(channel: u8) u16 {
    return ini.init(Hw{}, channel, &clock_inited);
}

export fn ra8_canfd_deinit(channel: u8) u16 {
    return ini.deinit(Hw{}, channel);
}
