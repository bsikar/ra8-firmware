//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the ra8_mipi_dsi.h command functions (RA8FW-630), replacing
//! ra8_mipi_dsi_cmd.c. The pending-RX globals stay in ra8_mipi_dsi.c.

const common = @import("abi_common.zig");
const cmd = @import("internal/mipi_dsi_cmd.zig");

const tag = "MIPI_DSI";
const base_addr: usize = 0x40346000;

extern var s_mipi_dsi_pending_rx_buffer: ?[*]u8;
extern var s_mipi_dsi_pending_rx_len: u16;

const Dsi = struct {
    pub fn read32(_: Dsi, off: u16) u32 {
        return @as(*volatile u32, @ptrFromInt(base_addr + off)).*;
    }
    pub fn write32(_: Dsi, off: u16, value: u32) void {
        @as(*volatile u32, @ptrFromInt(base_addr + off)).* = value;
    }
    pub fn errMsg(_: Dsi, msg: [*:0]const u8) void {
        common.ra8_log_emit_error(tag, msg);
    }
    pub fn errVal(_: Dsi, msg: [*:0]const u8, value: u16) void {
        common.ra8_log_emit_error_val(tag, msg, value);
    }
    pub fn setPendingRx(_: Dsi, buf: [*]u8, len: u16) void {
        s_mipi_dsi_pending_rx_buffer = buf;
        s_mipi_dsi_pending_rx_len = len;
    }
};

export fn ra8_mipi_dsi_send_command(c: ?*const cmd.Command) u16 {
    return cmd.sendCommand(Dsi{}, c);
}

export fn ra8_mipi_dsi_send_short_packet(cmd_id: u8, vc: u8, param0: u8, param1: u8) u16 {
    return cmd.sendShortPacket(Dsi{}, cmd_id, vc, param0, param1);
}

export fn ra8_mipi_dsi_send_long_packet(cmd_id: u8, vc: u8, data: ?[*]const u8, tx_len: u16, low_power: bool) u16 {
    return cmd.sendLongPacket(Dsi{}, cmd_id, vc, data, tx_len, low_power);
}

export fn ra8_mipi_dsi_read_packet(cmd_id: u8, vc: u8, param0: u8, param1: u8, p_rx_buffer: ?[*]u8, rx_len: u16) u16 {
    return cmd.readPacket(Dsi{}, cmd_id, vc, param0, param1, p_rx_buffer, rx_len);
}
