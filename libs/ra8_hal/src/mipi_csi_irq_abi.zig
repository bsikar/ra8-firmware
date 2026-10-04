//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the ra8_mipi_csi_isr.h functions (RA8FW-627), replacing
//! ra8_mipi_csi_irq.c.

const common = @import("abi_common.zig");
const irq = @import("internal/mipi_csi_irq.zig");

const tag = "MIPI_CSI";
const base_addr: usize = 0x40347000;

const Csi = struct {
    pub fn read32(_: Csi, off: u16) u32 {
        return @as(*volatile u32, @ptrFromInt(base_addr + off)).*;
    }
    pub fn write32(_: Csi, off: u16, value: u32) void {
        @as(*volatile u32, @ptrFromInt(base_addr + off)).* = value;
    }
    pub fn err(_: Csi, msg: [*:0]const u8) void {
        common.ra8_log_emit_error(tag, msg);
    }
};

var state: irq.State = .{};

export fn ra8_mipi_csi_attach_handler(f: irq.EventFn, ctx: ?*anyopaque) u16 {
    state.rx = .{ .f = f, .ctx = ctx };
    return common.k_ra8_ok;
}

export fn ra8_mipi_csi_attach_dl_handler(f: irq.LaneFn, ctx: ?*anyopaque) u16 {
    state.dl = .{ .f = f, .ctx = ctx };
    return common.k_ra8_ok;
}

export fn ra8_mipi_csi_attach_vc_handler(f: irq.VcFn, ctx: ?*anyopaque) u16 {
    state.vc = .{ .f = f, .ctx = ctx };
    return common.k_ra8_ok;
}

export fn ra8_mipi_csi_attach_pm_handler(f: irq.EventFn, ctx: ?*anyopaque) u16 {
    state.pm = .{ .f = f, .ctx = ctx };
    return common.k_ra8_ok;
}

export fn ra8_mipi_csi_attach_short_packet_handler(f: irq.EventFn, ctx: ?*anyopaque) u16 {
    state.gst = .{ .f = f, .ctx = ctx };
    return common.k_ra8_ok;
}

export fn ra8_mipi_csi_attach_error_handler(f: irq.ErrorFn, ctx: ?*anyopaque) u16 {
    state.err = .{ .f = f, .ctx = ctx };
    return common.k_ra8_ok;
}

export fn ra8_mipi_csi_dispatch() void {
    irq.dispatch(&state, Csi{});
}

export fn ra8_mipi_csi_dispatch_dl() void {
    irq.dispatchDl(&state, Csi{});
}

export fn ra8_mipi_csi_dispatch_vc() void {
    irq.dispatchVc(&state, Csi{});
}

export fn ra8_mipi_csi_dispatch_pm() void {
    irq.dispatchPm(&state, Csi{});
}

export fn ra8_mipi_csi_dispatch_short_packet() void {
    irq.dispatchShortPacket(&state, Csi{});
}

/// RA8_PRIV: ra8_mipi_csi.c calls this from its deinit path.
export fn priv_ra8_mipi_csi_detach_all_handlers() void {
    state.detachAll();
}

export fn ra8_mipi_csi_set_virtual_channels(vc_mask: u16) u16 {
    return irq.setVirtualChannels(&state, Csi{}, vc_mask);
}

export fn ra8_mipi_csi_set_data_format(vc: u8, format: u8) u16 {
    return irq.setDataFormat(&state, Csi{}, vc, format);
}
