//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the RIIC bit-rate solver and ra8_i2c_set_clock (RA8FW-695),
//! moved out of ra8_i2c_config.c. ra8_i2c_init (i2c_config_abi.zig, RA8FW-702) calls
//! priv_ra8_i2c_internal_bitrate through ra8_i2c_internal.h.

const common = @import("abi_common.zig");
const br = @import("internal/i2c_bitrate.zig");
const st = @import("internal/i2c_status.zig");

/// Owned by ra8_i2c.c, declared in ra8_i2c_internal.h.
extern const g_i2c_tag: [*:0]const u8;
extern fn priv_ra8_i2c_internal_clk_invalid(bus_hz: u32, pclkb_hz: u32) bool;

/// r_i2c_regs_t offsets (inc/ra8_i2c_regs.h).
const off_icmr1: usize = 0x02;
const off_icbrl: usize = 0x10;
const off_icbrh: usize = 0x11;

export fn priv_ra8_i2c_internal_bitrate(
    bus_hz: u32,
    pclkb_hz: u32,
    out_cks: ?*u8,
    out_brh: ?*u8,
    out_brl: ?*u8,
) u16 {
    const cks = out_cks orelse return nullArg("bitrate: out_cks");
    const brh = out_brh orelse return nullArg("bitrate: out_brh");
    if (priv_ra8_i2c_internal_clk_invalid(bus_hz, pclkb_hz)) return common.k_ra8_err_invalid_arg;
    const rate = br.solve(bus_hz, pclkb_hz);
    cks.* = rate.cks;
    brh.* = rate.brh;
    if (out_brl) |brl| brl.* = rate.brl;
    return common.k_ra8_ok;
}

export fn ra8_i2c_set_clock(channel: u8, bus_hz: u32, pclkb_hz: u32) u16 {
    const icsr2 = st.icsr2Addr(channel) orelse return common.k_ra8_err_invalid_arg;
    const base = icsr2 - st.off_icsr2;
    var cks: u8 = 0;
    var brh: u8 = 0;
    var brl: u8 = 0;
    const err = priv_ra8_i2c_internal_bitrate(bus_hz, pclkb_hz, &cks, &brh, &brl);
    if (err != common.k_ra8_ok) return err;
    const icmr1: *volatile u8 = @ptrFromInt(base + off_icmr1);
    icmr1.* = br.icmr1WithCks(icmr1.*, cks);
    @as(*volatile u8, @ptrFromInt(base + off_icbrl)).* = brl;
    @as(*volatile u8, @ptrFromInt(base + off_icbrh)).* = brh;
    return common.k_ra8_ok;
}

fn nullArg(message: [*:0]const u8) u16 {
    common.ra8_log_emit_error(g_i2c_tag, message);
    return common.k_ra8_err_null_ptr;
}
