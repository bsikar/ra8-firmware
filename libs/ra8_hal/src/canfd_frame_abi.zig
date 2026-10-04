//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for ra8_canfd_transmit / _receive / _get_error_state (RA8FW-587).

const common = @import("abi_common.zig");
const frame = @import("internal/canfd_frame.zig");
const timing = @import("internal/canfd_timing.zig");

const tag = "CANFD";

/// Volatile byte and word access at a CANFD channel base.
const Mmio = struct {
    base: usize,

    pub fn read8(self: Mmio, off: usize) u8 {
        return @as(*volatile u8, @ptrFromInt(self.base + off)).*;
    }
    pub fn write8(self: Mmio, off: usize, value: u8) void {
        @as(*volatile u8, @ptrFromInt(self.base + off)).* = value;
    }
    pub fn read32(self: Mmio, off: usize) u32 {
        return @as(*volatile u32, @ptrFromInt(self.base + off)).*;
    }
    pub fn write32(self: Mmio, off: usize, value: u32) void {
        @as(*volatile u32, @ptrFromInt(self.base + off)).* = value;
    }
};

/// RA8_CHECK_NULL_PTR: log `msg` and return null_ptr.
fn nullPtr(msg: [*:0]const u8) u16 {
    common.ra8_log_emit_error(tag, msg);
    return common.k_ra8_err_null_ptr;
}

fn mmio(channel: u8) ?Mmio {
    const base = timing.channelBase(channel) orelse return null;
    return .{ .base = base };
}

export fn ra8_canfd_transmit(channel: u8, f: ?*const frame.Frame) u16 {
    const regs = mmio(channel) orelse return nullPtr("channel out of range");
    const p = f orelse return nullPtr("frame must not be nullptr");
    return frame.transmit(regs, p);
}

export fn ra8_canfd_receive(channel: u8, out: ?*frame.Frame) u16 {
    const regs = mmio(channel) orelse return nullPtr("channel out of range");
    const p = out orelse return nullPtr("out_frame must not be nullptr");
    return frame.receive(regs, p);
}

export fn ra8_canfd_get_error_state(channel: u8, tx_err: ?*u8, rx_err: ?*u8) u16 {
    const regs = mmio(channel) orelse return nullPtr("channel out of range");
    const tx = tx_err orelse return nullPtr("tx_err must not be nullptr");
    const rx = rx_err orelse return nullPtr("rx_err must not be nullptr");
    const c = frame.errorCounters(regs);
    tx.* = c.tec;
    rx.* = c.rec;
    return common.k_ra8_ok;
}
