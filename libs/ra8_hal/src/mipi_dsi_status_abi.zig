//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the MIPI DSI-2 status / IRQ-enable surface (ra8_mipi_dsi_api.h),
//! RA8FW-648, replacing that block of ra8_mipi_dsi_dispatch.c. Layouts of
//! the out structs are asserted against the C sizes at comptime.

const std = @import("std");
const common = @import("abi_common.zig");
const st = @import("internal/mipi_dsi_status.zig");

const tag = "MIPI_DSI";
const base_addr: usize = 0x40346000;

comptime {
    std.debug.assert(@sizeOf(st.LinkStatus) == 5);
    std.debug.assert(@sizeOf(st.AckError) == 4);
    std.debug.assert(@offsetOf(st.AckError, "virtual_channel") == 2);
    std.debug.assert(@sizeOf(st.RxResult) == 12);
    std.debug.assert(@offsetOf(st.RxResult, "long_packet") == 4);
}

const Dsi = struct {
    pub fn read32(_: Dsi, off: u16) u32 {
        return @as(*volatile u32, @ptrFromInt(base_addr + off)).*;
    }
    pub fn write32(_: Dsi, off: u16, value: u32) void {
        @as(*volatile u32, @ptrFromInt(base_addr + off)).* = value;
    }
    /// RA8_CHECK_NULL_PTR's single log line.
    pub fn err(_: Dsi, msg: [*:0]const u8) void {
        common.ra8_log_emit_error(tag, msg);
    }
};

const dsi = Dsi{};

export fn ra8_mipi_dsi_get_status(out_mask: ?*u32) u16 {
    return st.getStatus(dsi, out_mask);
}

export fn ra8_mipi_dsi_link_status_get(out_status: ?*st.LinkStatus) u16 {
    return st.linkStatusGet(dsi, out_status);
}

export fn ra8_mipi_dsi_clear_status(mask: u32) u16 {
    return st.clearStatus(dsi, mask);
}

export fn ra8_mipi_dsi_ack_error_get(out_err: ?*st.AckError) u16 {
    return st.ackErrorGet(dsi, out_err);
}

export fn ra8_mipi_dsi_rx_result_get(slot: u8, out_result: ?*st.RxResult) u16 {
    return st.rxResultGet(dsi, slot, out_result);
}

export fn ra8_mipi_dsi_rx_payload_read(dest: ?[*]u8, max_len: u16, out_len: ?*u16) u16 {
    return st.rxPayloadRead(dsi, dest, max_len, out_len);
}

export fn ra8_mipi_dsi_te_event_pending(out_pending: ?*bool) u16 {
    return st.teEventPending(dsi, out_pending);
}

export fn ra8_mipi_dsi_te_event_clear() u16 {
    return st.teEventClear(dsi);
}

export fn ra8_mipi_dsi_irq_enable(event: u8, mask: u32, enable: bool) u16 {
    return st.irqEnable(dsi, event, mask, enable);
}
