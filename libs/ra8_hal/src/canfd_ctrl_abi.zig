//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for ra8_canfd_set_test_mode, ra8_canfd_set_iso_mode,
//! ra8_canfd_enter_stop and ra8_canfd_exit_stop (RA8FW-861). Logic lives in
//! internal/canfd_ctrl.zig; the channel-mode handshake stays in ra8_canfd.c.

const common = @import("abi_common.zig");
const ctrl = @import("internal/canfd_ctrl.zig");
const tdc = @import("internal/canfd_tdc.zig");

const tag = "CANFD";

extern fn priv_ra8_canfd_internal_set_channel_mode(reg: *volatile anyopaque, mode: c_uint) u16;
extern fn ra8_mstp_enable(id: u16) u16;
extern fn ra8_mstp_disable(id: u16) u16;

fn reg32(base: usize, off: usize) *volatile u32 {
    return @ptrFromInt(base + off);
}

/// Channel `base` bound to the C mode handshake and CTR.
const Hw = struct {
    base: usize,

    pub fn setMode(self: Hw, mode: ctrl.Mode) u16 {
        return priv_ra8_canfd_internal_set_channel_mode(@ptrFromInt(self.base), @backingInt(mode));
    }
    pub fn readCtr(self: Hw) u32 {
        return reg32(self.base, ctrl.off_ctr).*;
    }
    pub fn writeCtr(self: Hw, value: u32) void {
        reg32(self.base, ctrl.off_ctr).* = value;
    }
};

export fn ra8_canfd_set_test_mode(channel: u8, mode: c_uint) u16 {
    if (channel >= tdc.channel_bases.len) {
        common.ra8_log_emit_error(tag, "channel out of range");
        return common.k_ra8_err_null_ptr;
    }
    const m: u8 = if (mode > ctrl.ctms_max) ctrl.ctms_max + 1 else @intCast(mode);
    return ctrl.setTestMode(Hw{ .base = tdc.channel_bases[channel] }, m) catch common.k_ra8_err_invalid_arg;
}

export fn ra8_canfd_set_iso_mode(enable: bool) u16 {
    const cfg = reg32(tdc.channel_bases[0], ctrl.off_gfdcfg);
    cfg.* = ctrl.isoValue(cfg.*, enable);
    return common.k_ra8_ok;
}

export fn ra8_canfd_enter_stop(channel: u8) u16 {
    if (channel >= ctrl.mstp_ids.len) return common.k_ra8_err_invalid_arg;
    return ra8_mstp_disable(ctrl.mstp_ids[channel]);
}

export fn ra8_canfd_exit_stop(channel: u8) u16 {
    if (channel >= ctrl.mstp_ids.len) return common.k_ra8_err_invalid_arg;
    return ra8_mstp_enable(ctrl.mstp_ids[channel]);
}
