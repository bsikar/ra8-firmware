//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for ra8_canfd_filter_set (RA8FW-862). Logic lives in
//! internal/canfd_filter.zig; the global-mode handshake and the RX FIFO0
//! enable stay in ra8_canfd.c for now.

const common = @import("abi_common.zig");
const filter = @import("internal/canfd_filter.zig");
const tdc = @import("internal/canfd_tdc.zig");

extern fn priv_ra8_canfd_internal_set_global_mode(reg: *volatile anyopaque, gmdc_value: u32) u16;
extern fn priv_ra8_canfd_internal_enable_rx_fifo0(reg: *volatile anyopaque) void;

/// The AFL is global across instances and reached through channel 0.
const Hw = struct {
    const base = tdc.channel_bases[0];

    pub fn globalMode(_: Hw, mode: filter.GlobalMode) u16 {
        return priv_ra8_canfd_internal_set_global_mode(@ptrFromInt(base), @intFromEnum(mode));
    }
    pub fn read(_: Hw, off: usize) u32 {
        return @as(*volatile u32, @ptrFromInt(base + off)).*;
    }
    pub fn write(_: Hw, off: usize, value: u32) void {
        @as(*volatile u32, @ptrFromInt(base + off)).* = value;
    }
    pub fn enableRxFifo0(_: Hw) void {
        priv_ra8_canfd_internal_enable_rx_fifo0(@ptrFromInt(base));
    }
};

export fn ra8_canfd_filter_set(filter_id: u16, accept_id: u32, mask: u32, dlc: u8) u16 {
    return filter.set(Hw{}, filter_id, accept_id, mask, dlc) catch common.k_ra8_err_invalid_arg;
}
