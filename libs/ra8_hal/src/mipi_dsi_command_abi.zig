//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the MIPI DSI-2 command / ULPS / link-status convenience
//! surface (ra8_mipi_dsi_api.h), RA8FW-689. Replaces the last block of
//! ra8_mipi_dsi_dispatch.c, which is deleted. The packet senders, ULPS
//! entry/exit and link-status getter are the existing Zig exports.

const common = @import("abi_common.zig");
const cmd = @import("internal/mipi_dsi_command.zig");
const st = @import("internal/mipi_dsi_status.zig");

const tag = "MIPI_DSI";

extern fn ra8_mipi_dsi_send_short_packet(cmd_id: u8, vc: u8, param0: u8, param1: u8) u16;
extern fn ra8_mipi_dsi_send_long_packet(cmd_id: u8, vc: u8, data: ?[*]const u8, tx_len: u16, low_power: bool) u16;
extern fn ra8_mipi_dsi_ulps_enter(lanes: u8) u16;
extern fn ra8_mipi_dsi_ulps_exit(lanes: u8) u16;
extern fn ra8_mipi_dsi_link_status_get(out_status: ?*st.LinkStatus) u16;

const Tx = struct {
    pub fn short(_: Tx, dt: u8, vc: u8, p0: u8, p1: u8) u16 {
        return ra8_mipi_dsi_send_short_packet(dt, vc, p0, p1);
    }
    pub fn long(_: Tx, dt: u8, vc: u8, data: ?[*]const u8, len: u16, low_power: bool) u16 {
        return ra8_mipi_dsi_send_long_packet(dt, vc, data, len, low_power);
    }
    /// RA8_CHECK_NULL_PTR's single log line.
    pub fn err(_: Tx, msg: [*:0]const u8) void {
        common.ra8_log_emit_error(tag, msg);
    }
};

const tx = Tx{};

export fn ra8_mipi_dsi_send_command_short(dt: u8, params: ?*const [2]u8) u16 {
    return cmd.sendShort(tx, dt, params);
}

export fn ra8_mipi_dsi_send_command_long(dt: u8, payload: ?[*]const u8, len: u16) u16 {
    return cmd.sendLong(tx, dt, payload, len);
}

export fn ra8_mipi_dsi_send_command_payload(packet_type: u8, payload: ?[*]const u8, len: u16) u16 {
    return cmd.sendPayload(tx, packet_type, payload, len);
}

export fn ra8_mipi_dsi_enter_ulps() u16 {
    return ra8_mipi_dsi_ulps_enter(cmd.lane_all);
}

export fn ra8_mipi_dsi_exit_ulps() u16 {
    return ra8_mipi_dsi_ulps_exit(cmd.lane_all);
}

export fn ra8_mipi_dsi_get_link_status(out: ?*st.LinkStatus) u16 {
    return ra8_mipi_dsi_link_status_get(out);
}
