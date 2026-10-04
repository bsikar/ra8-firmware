//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for inc/ra8_acmphs.h (RA8FW-578). Module stop stays in C
//! behind externs.

const common = @import("abi_common.zig");
const acmphs = @import("internal/acmphs.zig");

const tag = "ACMPHS";

const EventFn = *const fn (ctx: ?*anyopaque, channel: u8) callconv(.C) void;

var handler: ?EventFn = null;
var handler_ctx: ?*anyopaque = null;

extern fn ra8_mstp_enable(id: u16) u16;
extern fn ra8_mstp_disable(id: u16) u16;

fn reg(channel: u8, offset: usize) *volatile u8 {
    return @ptrFromInt(acmphs.regAddr(channel, offset));
}

fn fail(err: u16, message: [*:0]const u8) u16 {
    common.ra8_log_emit_error(tag, message);
    common.ra8_log_emit_error_val(tag, "Error", err);
    return err;
}

fn nullPtr(message: [*:0]const u8) u16 {
    common.ra8_log_emit_error(tag, message);
    return common.k_ra8_err_null_ptr;
}

fn mstpEnable(channel: u8, message: [*:0]const u8) u16 {
    const id = acmphs.mstpId(channel) orelse return common.k_ra8_ok;
    const err = ra8_mstp_enable(id);
    if (err != common.k_ra8_ok) return fail(err, message);
    return common.k_ra8_ok;
}

fn resetChannel(channel: u8) u16 {
    const err = mstpEnable(channel, "acmphs_init: mstp enable");
    if (err != common.k_ra8_ok) return err;
    reg(channel, acmphs.off_cmpctl).* = 0;
    reg(channel, acmphs.off_cmpsel0).* = 0;
    reg(channel, acmphs.off_cmpsel1).* = 0;
    reg(channel, acmphs.off_cpioc).* = 0;
    return common.k_ra8_ok;
}

export fn ra8_acmphs_init() u16 {
    var ch: u8 = 0;
    while (ch < acmphs.channel_count) : (ch += 1) {
        const err = resetChannel(ch);
        if (err != common.k_ra8_ok) return fail(err, "acmphs_init channel reset");
    }
    common.ra8_log_emit_info(tag, "acmphs_init");
    return common.k_ra8_ok;
}

export fn ra8_acmphs_channel_enable(channel: u8) u16 {
    if (!acmphs.channelOk(channel)) return common.k_ra8_err_invalid_arg;
    const ctl = reg(channel, acmphs.off_cmpctl);
    ctl.* = ctl.* | acmphs.mask_hcen;
    common.ra8_log_emit_info_val(tag, "enable channel", channel);
    return common.k_ra8_ok;
}

export fn ra8_acmphs_read_output(channel: u8, out: ?*u8) u16 {
    const o = out orelse return nullPtr("out must not be nullptr");
    if (!acmphs.channelOk(channel)) return common.k_ra8_err_invalid_arg;
    o.* = acmphs.levelOf(reg(channel, acmphs.off_cmpmon).*);
    return common.k_ra8_ok;
}

export fn ra8_acmphs_channel_init(channel: u8, cfg: ?*const acmphs.Cfg) u16 {
    const c = cfg orelse return nullPtr("cfg must not be nullptr");
    if (!acmphs.channelOk(channel)) return common.k_ra8_err_invalid_arg;
    const err = mstpEnable(channel, "acmphs_init: mstp");
    if (err != common.k_ra8_ok) return err;
    reg(channel, acmphs.off_cmpsel0).* = c.ivpsel;
    reg(channel, acmphs.off_cmpsel1).* = c.ivrefsel;
    reg(channel, acmphs.off_cmpctl).* = acmphs.packCtl(c.*);
    return common.k_ra8_ok;
}

export fn ra8_acmphs_channel_deinit(channel: u8) u16 {
    if (!acmphs.channelOk(channel)) return common.k_ra8_err_invalid_arg;
    reg(channel, acmphs.off_cmpctl).* = 0;
    reg(channel, acmphs.off_cmpsel0).* = 0;
    reg(channel, acmphs.off_cmpsel1).* = 0;
    if (acmphs.mstpId(channel)) |id| _ = ra8_mstp_disable(id);
    return common.k_ra8_ok;
}

export fn ra8_acmphs_set_inputs(channel: u8, ivpsel: u8, ivrefsel: u8) u16 {
    if (!acmphs.channelOk(channel)) return common.k_ra8_err_invalid_arg;
    reg(channel, acmphs.off_cmpsel0).* = ivpsel;
    reg(channel, acmphs.off_cmpsel1).* = ivrefsel;
    return common.k_ra8_ok;
}

export fn ra8_acmphs_get_status(channel: u8, out_mask: ?*u8) u16 {
    const o = out_mask orelse return nullPtr("out_mask must not be nullptr");
    if (!acmphs.channelOk(channel)) return common.k_ra8_err_invalid_arg;
    o.* = reg(channel, acmphs.off_cmpctl).* & acmphs.ctl_mask;
    return common.k_ra8_ok;
}

export fn ra8_acmphs_clear_status(channel: u8) u16 {
    if (!acmphs.channelOk(channel)) return common.k_ra8_err_invalid_arg;
    reg(channel, acmphs.off_cmpctl).* = 0;
    return common.k_ra8_ok;
}

export fn ra8_acmphs_attach_handler(fn_ptr: ?EventFn, ctx: ?*anyopaque) u16 {
    handler = fn_ptr;
    handler_ctx = ctx;
    return common.k_ra8_ok;
}

export fn ra8_acmphs_enter_stop(channel: u8) u16 {
    if (!acmphs.channelOk(channel)) return common.k_ra8_err_invalid_arg;
    const id = acmphs.mstpId(channel) orelse return common.k_ra8_ok;
    return ra8_mstp_disable(id);
}

export fn ra8_acmphs_exit_stop(channel: u8) u16 {
    if (!acmphs.channelOk(channel)) return common.k_ra8_err_invalid_arg;
    const id = acmphs.mstpId(channel) orelse return common.k_ra8_ok;
    return ra8_mstp_enable(id);
}

/// ISR-safe: reads the handler pair once, then calls it.
export fn ra8_acmphs_dispatch(channel: u8) void {
    if (!acmphs.channelOk(channel)) return;
    const f = handler;
    const ctx = handler_ctx;
    if (f) |call| call(ctx, channel);
}
