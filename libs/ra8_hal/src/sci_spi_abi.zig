//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for inc/ra8_sci_spi.h (RA8FW-579). Module stop stays in C
//! behind externs.

const common = @import("abi_common.zig");
const spi = @import("internal/sci_spi.zig");

const tag = "SCI_SPI";

extern fn ra8_mstp_enable(id: u16) u16;
extern fn ra8_mstp_disable(id: u16) u16;

fn reg(channel: u8, offset: usize) *volatile u32 {
    return @ptrFromInt(spi.regAddr(channel, offset));
}

/// Same contract as ra8_hw_wait_flag_set32 (a static inline in C).
fn waitSet(r: *volatile u32, mask: u32) u16 {
    var i: u32 = 0;
    while (i < spi.wait_budget) : (i += 1) {
        if (r.* & mask != 0) return common.k_ra8_ok;
    }
    return common.k_ra8_err_hw_timeout;
}

fn applyRate(channel: u8, baud_hz: u32, pclk_hz: u32) void {
    reg(channel, spi.off_ccr2).* = spi.ccr2(baud_hz, pclk_hz);
}

export fn ra8_sci_spi_init(channel: u8, cfg: ?*const spi.Cfg) u16 {
    const c = cfg orelse {
        common.ra8_log_emit_error(tag, "spi_init: cfg");
        return common.k_ra8_err_null_ptr;
    };
    if (!spi.channelOk(channel) or c.baud_hz == 0) return common.k_ra8_err_invalid_arg;
    const err = ra8_mstp_enable(spi.mstpId(channel));
    if (err != common.k_ra8_ok) {
        common.ra8_log_emit_error(tag, "spi_init: mstp");
        common.ra8_log_emit_error_val(tag, "Error", err);
        return err;
    }
    reg(channel, spi.off_ccr0).* = 0;
    reg(channel, spi.off_ccr1).* = 0;
    reg(channel, spi.off_ccr3).* = spi.ccr3(c.*);
    applyRate(channel, c.baud_hz, c.pclk_hz);
    reg(channel, spi.off_ccr4).* = 0;
    reg(channel, spi.off_cfclr).* = spi.cfclr_default;
    reg(channel, spi.off_ccr0).* = spi.ccr0_te_re;
    return common.k_ra8_ok;
}

export fn ra8_sci_spi_deinit(channel: u8) u16 {
    if (!spi.channelOk(channel)) return common.k_ra8_err_invalid_arg;
    reg(channel, spi.off_ccr0).* = 0;
    return ra8_mstp_disable(spi.mstpId(channel));
}

/// CCR0 drops while CKS/BRR change, then TE/RE come back as they were.
export fn ra8_sci_spi_set_clock(channel: u8, baud_hz: u32, pclk_hz: u32) u16 {
    if (!spi.channelOk(channel) or baud_hz == 0) return common.k_ra8_err_invalid_arg;
    const ccr0 = reg(channel, spi.off_ccr0);
    const saved = ccr0.*;
    ccr0.* = 0;
    applyRate(channel, baud_hz, pclk_hz);
    ccr0.* = saved;
    return common.k_ra8_ok;
}

export fn ra8_sci_spi_xfer8(channel: u8, tx: u8, rx: ?*u8) u16 {
    if (!spi.channelOk(channel)) {
        common.ra8_log_emit_error(tag, "spi_xfer8: channel out of range");
        return common.k_ra8_err_null_ptr;
    }
    const csr = reg(channel, spi.off_csr);
    var err = waitSet(csr, spi.csr_tdre);
    if (err != common.k_ra8_ok) return err;
    reg(channel, spi.off_tdr).* = tx;
    err = waitSet(csr, spi.csr_rdrf);
    if (err != common.k_ra8_ok) return err;
    const received: u8 = @truncate(reg(channel, spi.off_rdr).* & spi.rdr_data8);
    reg(channel, spi.off_cfclr).* = spi.cfclr_rdrfc;
    if (rx) |p| p.* = received;
    return common.k_ra8_ok;
}

export fn ra8_sci_spi_xfer(channel: u8, tx: ?[*]const u8, rx: ?[*]u8, len: u32) u16 {
    if (!spi.channelOk(channel)) return common.k_ra8_err_invalid_arg;
    var i: u32 = 0;
    while (i < len) : (i += 1) {
        const out = if (tx) |t| t[i] else spi.idle_byte;
        var in: u8 = 0;
        const err = ra8_sci_spi_xfer8(channel, out, &in);
        if (err != common.k_ra8_ok) return err;
        if (rx) |r| r[i] = in;
    }
    return common.k_ra8_ok;
}
