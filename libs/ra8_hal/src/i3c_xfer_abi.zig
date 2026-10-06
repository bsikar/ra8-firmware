//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for I3C write, read and transfer (RA8FW-823). Native mode runs
//! internal/i3c_xfer.zig; I2C mode goes to ra8_i3c_i2c_*.

const common = @import("abi_common.zig");
const xfer = @import("internal/i3c_xfer.zig");
const ccc = @import("internal/i3c_ccc.zig");
const ctl = @import("internal/i3c_ctl.zig");

const tag = "I3C";
const mode_i2c: u8 = 1;
const Chan = extern struct { initialized: bool, mode: u8 };

extern var s_i3c_chan: [1]Chan;
extern fn ra8_i3c_i2c_write(channel: u8, addr: u8, data: ?[*]const u8, len: u32, restart: bool) u16;
extern fn ra8_i3c_i2c_read(channel: u8, addr: u8, buf: ?[*]u8, len: u32, restart: bool) u16;
extern fn ra8_i3c_i2c_transfer(channel: u8, addr: u8, wr: ?[*]const u8, wr_len: u32, rd: ?[*]u8, rd_len: u32) u16;

const Mmio = struct {
    pub fn read32(_: Mmio, off: usize) u32 {
        const p: *volatile u32 = @ptrFromInt(ctl.base + off);
        return p.*;
    }
    pub fn write32(_: Mmio, off: usize, v: u32) void {
        const p: *volatile u32 = @ptrFromInt(ctl.base + off);
        p.* = v;
    }
};

/// Channel range then initialized; null when the native path may run.
fn gate(channel: u8) ?u16 {
    if (channel >= s_i3c_chan.len) return common.k_ra8_err_invalid_arg;
    if (!s_i3c_chan[channel].initialized) return common.k_ra8_err_invalid_state;
    return null;
}

export fn ra8_i3c_write(channel: u8, addr: u8, data: ?[*]const u8, len: u32, restart: bool) u16 {
    if (gate(channel)) |rc| return rc;
    if (s_i3c_chan[channel].mode == mode_i2c) return ra8_i3c_i2c_write(channel, addr, data, len, restart);
    if (addr > ccc.addr_mask) return common.k_ra8_err_invalid_arg;
    if (len > 0 and data == null) return common.k_ra8_err_null_ptr;
    if (len > xfer.length_max) return common.k_ra8_err_invalid_arg;
    const bytes: []const u8 = if (data) |d| d[0..len] else &.{};
    xfer.write(Mmio{}, addr, bytes);
    return common.k_ra8_ok;
}

export fn ra8_i3c_read(channel: u8, addr: u8, buf: ?[*]u8, len: u32, restart: bool) u16 {
    if (gate(channel)) |rc| return rc;
    if (s_i3c_chan[channel].mode == mode_i2c) return ra8_i3c_i2c_read(channel, addr, buf, len, restart);
    const b = buf orelse {
        common.ra8_log_emit_error(tag, "buf must not be nullptr");
        return common.k_ra8_err_null_ptr;
    };
    if (addr > ccc.addr_mask) return common.k_ra8_err_invalid_arg;
    if (len == 0 or len > xfer.length_max) return common.k_ra8_err_invalid_arg;
    xfer.read(Mmio{}, addr, b[0..len]);
    return common.k_ra8_ok;
}

export fn ra8_i3c_transfer(channel: u8, addr: u8, wr: ?[*]const u8, wr_len: u32, rd: ?[*]u8, rd_len: u32) u16 {
    if (channel >= s_i3c_chan.len) return common.k_ra8_err_invalid_arg;
    if (s_i3c_chan[channel].mode != mode_i2c) return common.k_ra8_err_invalid_state;
    return ra8_i3c_i2c_transfer(channel, addr, wr, wr_len, rd, rd_len);
}
