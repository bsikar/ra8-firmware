//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for ra8_i2c_write / read / transfer / scan (RA8FW-887). Owns the
//! per-channel state table and the "I2C" log tag that the config, clock,
//! status and target units declare extern (src/ra8_i2c_internal.h).
//! Logic lives in internal/i2c_xfer.zig.

const std = @import("std");
const builtin = @import("builtin");
const common = @import("abi_common.zig");
const cfg = @import("internal/i2c_config.zig");
const xfer = @import("internal/i2c_xfer.zig");

const hosted = builtin.os.tag != .freestanding;
const seam = struct {
    extern fn ra8_fake_mmio_poll(reg: *const volatile anyopaque, iter: u32, flag_set: bool) bool;
};

/// Mirrors `ra8_i2c_state_t` (src/ra8_i2c_internal.h) field for field.
pub const State = extern struct {
    initialized: bool,
    bus_held: bool,
    peripheral_active: bool,
    peripheral_handler: ?*const anyopaque,
    peripheral_ctx: ?*anyopaque,
};

export var s_i2c_state: [cfg.channel_count]State = std.mem.zeroes([cfg.channel_count]State);
export const g_i2c_tag: [*:0]const u8 = "I2C";

const Regs = struct {
    base: usize,

    fn ptr(self: Regs, off: usize) *volatile u8 {
        return @ptrFromInt(self.base + off);
    }
    pub fn read8(self: Regs, off: usize) u8 {
        return self.ptr(off).*;
    }
    pub fn write8(self: Regs, off: usize, v: u8) void {
        self.ptr(off).* = v;
    }
    pub fn poll(self: Regs, off: usize, iter: u32, cond: bool) bool {
        return if (hosted) seam.ra8_fake_mmio_poll(self.ptr(off), iter, cond) else cond;
    }
};

fn regs(channel: u8) ?Regs {
    const addr = cfg.regsAddr(channel) orelse return null;
    return .{ .base = addr };
}

fn fail(message: [*:0]const u8) u16 {
    common.ra8_log_emit_error(g_i2c_tag, message);
    return common.k_ra8_err_null_ptr;
}

export fn priv_ra8_i2c_internal_clk_invalid(bus_hz: u32, pclkb_hz: u32) bool {
    return bus_hz == 0 or pclkb_hz == 0;
}

export fn ra8_i2c_write(channel: u8, addr7: u8, data: ?[*]const u8, len: u32, send_stop: bool) u16 {
    const r = regs(channel) orelse return fail("i2c_write: channel");
    const d = data orelse return fail("i2c_write: data");
    return xfer.write(r, &s_i2c_state[channel].bus_held, addr7, d[0..len], send_stop);
}

export fn ra8_i2c_read(channel: u8, addr7: u8, data: ?[*]u8, len: u32) u16 {
    const r = regs(channel) orelse return fail("i2c_read: channel");
    const d = data orelse return fail("i2c_read: data");
    if (len == 0) return common.k_ra8_err_invalid_arg;
    return xfer.read(r, &s_i2c_state[channel].bus_held, addr7, d[0..len]);
}

/// Write (holding the bus when a read follows), then read. Checks do not
/// log, as before.
export fn ra8_i2c_transfer(channel: u8, addr7: u8, wr: ?[*]const u8, wr_len: u32, rd: ?[*]u8, rd_len: u32) u16 {
    if (cfg.regsAddr(channel) == null) return common.k_ra8_err_null_ptr;
    if (wr_len == 0 and rd_len == 0) return common.k_ra8_err_invalid_arg;
    if (wr_len != 0 and wr == null) return common.k_ra8_err_null_ptr;
    if (rd_len != 0 and rd == null) return common.k_ra8_err_null_ptr;
    if (wr_len != 0) {
        const rc = ra8_i2c_write(channel, addr7, wr, wr_len, rd_len == 0);
        if (rc != common.k_ra8_ok) return rc;
    }
    if (rd_len != 0) return ra8_i2c_read(channel, addr7, rd, rd_len);
    return common.k_ra8_ok;
}

export fn ra8_i2c_scan(channel: u8, addr7: u8, out_acked: ?*bool) u16 {
    const r = regs(channel) orelse return fail("i2c_scan: channel");
    const acked = out_acked orelse return fail("i2c_scan: out_acked");
    return xfer.scan(r, &s_i2c_state[channel].bus_held, addr7, acked);
}
