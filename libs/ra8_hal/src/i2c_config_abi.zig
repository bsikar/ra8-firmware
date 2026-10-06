//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for ra8_i2c_init / ra8_i2c_deinit (RA8FW-702), the last part of
//! ra8_i2c_config.c. The per-channel state table is defined in
//! i2c_xfer_abi.zig, shared with the transfer and peripheral planes.

const common = @import("abi_common.zig");
const cfg = @import("internal/i2c_config.zig");

/// Mirrors `ra8_i2c_state_t` (src/ra8_i2c_internal.h) field for field.
pub const State = extern struct {
    initialized: bool,
    bus_held: bool,
    peripheral_active: bool,
    peripheral_handler: ?*const anyopaque,
    peripheral_ctx: ?*anyopaque,
};

/// Mirrors `ra8_i2c_cfg_t` (inc/ra8_i2c.h).
pub const Cfg = extern struct {
    bus_hz: u32,
    pclkb_hz: u32,
};

/// Owned by i2c_xfer_abi.zig, declared in ra8_i2c_internal.h.
extern const g_i2c_tag: [*:0]const u8;
extern var s_i2c_state: [cfg.channel_count]State;
/// Exported by i2c_clock_abi.zig (RA8FW-695).
extern fn priv_ra8_i2c_internal_bitrate(bus_hz: u32, pclkb_hz: u32, out_cks: ?*u8, out_brh: ?*u8, out_brl: ?*u8) u16;
extern fn ra8_mstp_enable(id: u16) u16;
extern fn ra8_mstp_disable(id: u16) u16;

/// RA8_RETURN_ON_ERROR: log the message and the code, hand the code back.
fn fail(err: u16, message: [*:0]const u8) u16 {
    common.ra8_log_emit_error(g_i2c_tag, message);
    common.ra8_log_emit_error_val(g_i2c_tag, "Error", err);
    return err;
}

fn regs(channel: u8) ?[*]volatile u8 {
    const addr = cfg.regsAddr(channel) orelse return null;
    return @ptrFromInt(addr);
}

export fn ra8_i2c_init(channel: u8, config: ?*const Cfg) u16 {
    const c = config orelse {
        common.ra8_log_emit_error(g_i2c_tag, "i2c_init: cfg");
        return common.k_ra8_err_null_ptr;
    };
    var rate: cfg.Rate = .{ .cks = 0, .brh = 0, .brl = 0 };
    const br_err = priv_ra8_i2c_internal_bitrate(c.bus_hz, c.pclkb_hz, &rate.cks, &rate.brh, &rate.brl);
    if (br_err != common.k_ra8_ok) return fail(br_err, "i2c_init: bitrate");

    const reg = regs(channel) orelse return common.k_ra8_err_invalid_arg;
    const mst_err = ra8_mstp_enable(cfg.mstpId(channel));
    if (mst_err != common.k_ra8_ok) return fail(mst_err, "i2c_init: mstp");

    cfg.applyInit(reg, rate, c.bus_hz >= cfg.fast_plus_hz);
    s_i2c_state[channel].initialized = true;
    s_i2c_state[channel].bus_held = false;
    common.ra8_log_emit_info_val(g_i2c_tag, "i2c_init channel", channel);
    return common.k_ra8_ok;
}

export fn ra8_i2c_deinit(channel: u8) u16 {
    const reg = regs(channel) orelse return common.k_ra8_err_invalid_arg;
    // ICE = 0 puts SCL/SDA back in the inactive state (HUM 39.2.1, p 2369).
    reg[cfg.off_iccr1] = 0;
    s_i2c_state[channel].initialized = false;
    s_i2c_state[channel].bus_held = false;
    return ra8_mstp_disable(cfg.mstpId(channel));
}
