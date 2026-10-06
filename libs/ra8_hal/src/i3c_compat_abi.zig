//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the I3C I2C-compatibility delegators (RA8FW-824): each guards
//! the channel and forwards to ra8_i3c_i2c_*.

const common = @import("abi_common.zig");
const compat = @import("internal/i3c_compat.zig");

const tag = "I3C";
const Chan = compat.Chan;
/// `ra8_i3c_peripheral_cfg_t` and `ra8_i3c_i2c_peripheral_cfg_t` share this shape.
const PeripheralCfg = extern struct { peripheral_addr_7b: u8, general_call: u8 };

comptime {
    if (@sizeOf(PeripheralCfg) != 2) @compileError("peripheral cfg is 2 bytes");
}

extern var s_i3c_chan: [1]Chan;
extern fn ra8_i3c_i2c_set_clock(channel: u8, bus_hz: u32, pclka_hz: u32) u16;
extern fn ra8_i3c_i2c_scan(channel: u8, addr: u8, out_acked: ?*bool) u16;
extern fn ra8_i3c_i2c_get_errors(channel: u8, out_mask: ?*u8) u16;
extern fn ra8_i3c_i2c_clear_errors(channel: u8) u16;
extern fn ra8_i3c_i2c_abort(channel: u8) u16;
extern fn ra8_i3c_i2c_peripheral_open(channel: u8, cfg: *const PeripheralCfg) u16;
extern fn ra8_i3c_i2c_peripheral_close(channel: u8) u16;
extern fn ra8_i3c_i2c_peripheral_send(channel: u8, data: ?[*]const u8, len: u32) u16;
extern fn ra8_i3c_i2c_peripheral_receive(channel: u8, buf: ?[*]u8, len: u32) u16;
extern fn ra8_i3c_i2c_peripheral_status(channel: u8, out_mask: ?*u8) u16;

fn guard(channel: u8) ?u16 {
    return compat.guard(&s_i3c_chan, channel);
}

export fn ra8_i3c_set_clock(channel: u8, bus_hz: u32, pclka_hz: u32) u16 {
    if (guard(channel)) |rc| return rc;
    return ra8_i3c_i2c_set_clock(channel, bus_hz, pclka_hz);
}

export fn ra8_i3c_scan(channel: u8, addr: u8, out_acked: ?*bool) u16 {
    if (guard(channel)) |rc| return rc;
    return ra8_i3c_i2c_scan(channel, addr, out_acked);
}

export fn ra8_i3c_get_errors(channel: u8, out_mask: ?*u8) u16 {
    if (guard(channel)) |rc| return rc;
    return ra8_i3c_i2c_get_errors(channel, out_mask);
}

export fn ra8_i3c_clear_errors(channel: u8) u16 {
    if (guard(channel)) |rc| return rc;
    return ra8_i3c_i2c_clear_errors(channel);
}

export fn ra8_i3c_abort(channel: u8) u16 {
    if (guard(channel)) |rc| return rc;
    return ra8_i3c_i2c_abort(channel);
}

/// Responder open is a self-contained bring-up: on success the channel is
/// marked I2C and initialized so the other peripheral calls pass the guard.
export fn ra8_i3c_peripheral_open(channel: u8, cfg: ?*const PeripheralCfg) u16 {
    const c = cfg orelse {
        common.ra8_log_emit_error(tag, "peripheral_open: cfg");
        return common.k_ra8_err_null_ptr;
    };
    if (channel >= s_i3c_chan.len) return common.k_ra8_err_invalid_arg;
    const bcfg = PeripheralCfg{ .peripheral_addr_7b = c.peripheral_addr_7b, .general_call = c.general_call };
    const rc = ra8_i3c_i2c_peripheral_open(channel, &bcfg);
    if (rc == common.k_ra8_ok) s_i3c_chan[channel] = .{ .initialized = true, .mode = compat.mode_i2c };
    return rc;
}

export fn ra8_i3c_peripheral_close(channel: u8) u16 {
    if (guard(channel)) |rc| return rc;
    return ra8_i3c_i2c_peripheral_close(channel);
}

export fn ra8_i3c_peripheral_send(channel: u8, data: ?[*]const u8, len: u32) u16 {
    if (guard(channel)) |rc| return rc;
    return ra8_i3c_i2c_peripheral_send(channel, data, len);
}

export fn ra8_i3c_peripheral_receive(channel: u8, buf: ?[*]u8, len: u32) u16 {
    if (guard(channel)) |rc| return rc;
    return ra8_i3c_i2c_peripheral_receive(channel, buf, len);
}

export fn ra8_i3c_peripheral_status(channel: u8, out_mask: ?*u8) u16 {
    if (guard(channel)) |rc| return rc;
    return ra8_i3c_i2c_peripheral_status(channel, out_mask);
}
