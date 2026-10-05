//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for ra8_sci_lin.h (RA8FW-754), replacing ra8_sci_lin.c. Logic is in
//! internal/sci_lin.zig; this file owns the volatile SCI access, the polled
//! waits and the byte I/O through ra8_sci_putc/getc_polling.

const builtin = @import("builtin");
const common = @import("abi_common.zig");
const lin = @import("internal/sci_lin.zig");

const tag = "LIN";
const hw = Hw{};
const ok = common.k_ra8_ok;
const budget_long: u32 = 0x0004_0000;

extern fn ra8_sci_init(channel: u8, cfg: *const lin.UartCfg) u16;
extern fn ra8_sci_putc_polling(channel: u8, byte: u8) u16;
extern fn ra8_sci_getc_polling(channel: u8, out: *u8) u16;

/// Host C tests arm failures through this seam, as the C inline waits do
/// under UNIT_TEST. Freestanding builds never see it.
const hosted = builtin.os.tag != .freestanding;
const seam = struct {
    extern fn ra8_fake_mmio_wait_eval(reg: *const volatile anyopaque, iter: u32, real_cond: bool) bool;
};

const Hw = struct {
    pub fn read32(_: Hw, addr: usize) u32 {
        return @as(*volatile u32, @ptrFromInt(addr)).*;
    }
    pub fn write32(_: Hw, addr: usize, value: u32) void {
        @as(*volatile u32, @ptrFromInt(addr)).* = value;
    }
};

fn wait(addr: usize, mask: u32, want_set: bool) u16 {
    const reg: *volatile u32 = @ptrFromInt(addr);
    var i: u32 = 0;
    while (i < budget_long) : (i += 1) {
        const cond = ((reg.* & mask) != 0) == want_set;
        const done = if (hosted) seam.ra8_fake_mmio_wait_eval(reg, i, cond) else cond;
        if (done) return ok;
    }
    return common.k_ra8_err_hw_timeout;
}

fn nullPtr(msg: [*:0]const u8) u16 {
    common.ra8_log_emit_error(tag, msg);
    return common.k_ra8_err_null_ptr;
}

/// RA8_RETURN_ON_ERROR: the message, then "Error" with the code.
fn failed(err: u16, msg: [*:0]const u8) bool {
    if (err == ok) return false;
    common.ra8_log_emit_error(tag, msg);
    common.ra8_log_emit_error_val(tag, "Error", err);
    return true;
}

export fn ra8_sci_lin_init(channel: u8, cfg: ?*const lin.Cfg) u16 {
    const c = cfg orelse return nullPtr("lin_init: cfg");
    if (!lin.channelOk(channel)) return nullPtr("lin_init: channel out of range");
    if (!lin.cfgOk(c.*)) return common.k_ra8_err_invalid_arg;
    const err = ra8_sci_init(channel, &c.uart);
    if (failed(err, "lin_init: base uart")) return err;
    lin.programMode(hw, channel, c.role, c.timer_clk, c.break_field_len);
    common.ra8_log_emit_info_val(tag, "lin_init channel", channel);
    return ok;
}

export fn ra8_sci_lin_send_break(channel: u8) u16 {
    if (!lin.channelOk(channel)) return nullPtr("lin_send_break: channel out of range");
    const addr = lin.regAddr(channel, lin.off_xcr1);
    hw.write32(addr, lin.xcr1_tcst);
    return wait(addr, lin.xcr1_tcst, false);
}

export fn ra8_sci_lin_send_header(channel: u8, id: u8) u16 {
    if (!lin.channelOk(channel)) return nullPtr("lin_send_header: channel out of range");
    if (id > lin.id_max) return common.k_ra8_err_invalid_arg;
    const brk = ra8_sci_lin_send_break(channel);
    if (failed(brk, "lin_send_header: break")) return brk;
    const sync = ra8_sci_putc_polling(channel, lin.sync_byte);
    if (failed(sync, "lin_send_header: sync")) return sync;
    const p = ra8_sci_putc_polling(channel, lin.pid(id));
    if (failed(p, "lin_send_header: pid")) return p;
    return ok;
}

export fn ra8_sci_lin_break_detected(channel: u8, out_detected: ?*bool) u16 {
    const out = out_detected orelse return nullPtr("lin_break_detected: out_detected");
    if (!lin.channelOk(channel)) return nullPtr("lin_break_detected: channel out of range");
    out.* = (hw.read32(lin.regAddr(channel, lin.off_xsr0)) & lin.xsr0_bfdf) != 0;
    return ok;
}

export fn ra8_sci_lin_wait_break(channel: u8) u16 {
    if (!lin.channelOk(channel)) return nullPtr("lin_wait_break: channel out of range");
    return wait(lin.regAddr(channel, lin.off_xsr0), lin.xsr0_bfdf, true);
}

export fn ra8_sci_lin_clear_status(channel: u8) u16 {
    if (!lin.channelOk(channel)) return nullPtr("lin_clear_status: channel out of range");
    hw.write32(lin.regAddr(channel, lin.off_xfclr), lin.xfclr_default);
    return ok;
}

fn lenOk(len: u8) bool {
    return len != 0 and len <= lin.data_max;
}

export fn ra8_sci_lin_send_response(channel: u8, mode: u8, p: u8, data: ?[*]const u8, len: u8) u16 {
    if (!lin.channelOk(channel)) return nullPtr("lin_send_response: channel out of range");
    if (!lenOk(len)) return common.k_ra8_err_invalid_arg;
    var sum: u8 = 0;
    const cerr = ra8_sci_lin_checksum(mode, p, data, len, &sum);
    if (failed(cerr, "lin_send_response: checksum")) return cerr;
    const d = data orelse return nullPtr("lin_tx_buf: data");
    for (d[0..len]) |byte| {
        const err = ra8_sci_putc_polling(channel, byte);
        if (failed(err, "lin_tx_buf")) {
            _ = failed(err, "lin_send_response: data");
            return err;
        }
    }
    return ra8_sci_putc_polling(channel, sum);
}

export fn ra8_sci_lin_read_response(channel: u8, out_data: ?[*]u8, len: u8, out_checksum: ?*u8) u16 {
    if (!lin.channelOk(channel)) return nullPtr("lin_read_response: channel out of range");
    const d = out_data orelse return nullPtr("lin_read_response: out_data");
    const sum = out_checksum orelse return nullPtr("lin_read_response: out_checksum");
    if (!lenOk(len)) return common.k_ra8_err_invalid_arg;
    for (d[0..len]) |*byte| {
        const err = ra8_sci_getc_polling(channel, byte);
        if (failed(err, "lin_rx_buf")) {
            _ = failed(err, "lin_read_response: data");
            return err;
        }
    }
    return ra8_sci_getc_polling(channel, sum);
}

export fn ra8_sci_lin_pid(id: u8) u8 {
    return lin.pid(id);
}

export fn ra8_sci_lin_checksum(mode: u8, p: u8, data: ?[*]const u8, len: u8, out_checksum: ?*u8) u16 {
    const out = out_checksum orelse return nullPtr("lin_checksum: out_checksum");
    if (data == null and len != 0) return common.k_ra8_err_null_ptr;
    if (mode > lin.checksum_enhanced) return common.k_ra8_err_invalid_arg;
    const bytes: []const u8 = if (data) |d| d[0..len] else &.{};
    out.* = lin.checksum(mode, p, bytes);
    return ok;
}

export fn ra8_sci_lin_check_header(sync: u8, p: u8, out_id: ?*u8, out_valid: ?*bool) u16 {
    const id = out_id orelse return nullPtr("lin_check_header: out_id");
    const valid = out_valid orelse return nullPtr("lin_check_header: out_valid");
    const h = lin.checkHeader(sync, p);
    id.* = h.id;
    valid.* = h.valid;
    return ok;
}

export fn ra8_sci_lin_check_response(mode: u8, p: u8, data: ?[*]const u8, len: u8, received: u8, out_valid: ?*bool) u16 {
    const valid = out_valid orelse return nullPtr("lin_check_response: out_valid");
    var expected: u8 = 0;
    const cerr = ra8_sci_lin_checksum(mode, p, data, len, &expected);
    if (failed(cerr, "lin_check_response: checksum")) return cerr;
    valid.* = expected == received;
    return ok;
}
