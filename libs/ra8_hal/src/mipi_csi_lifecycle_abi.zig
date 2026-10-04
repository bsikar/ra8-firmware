//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for ra8_mipi_csi_init / deinit / reset / start_receive /
//! stop_receive (ra8_mipi_csi_ctrl.h), RA8FW-641. With this the last of
//! ra8_mipi_csi.c is in Zig and the C file is gone.

const common = @import("abi_common.zig");
const lc = @import("internal/mipi_csi_lifecycle.zig");

const tag = "MIPI_CSI";
const base_addr: usize = 0x40347000;
/// `ra8_mstp_t` k_ra8_mstp_mipi_csi: (k_ra8_mstp_reg_c << 8) | 17 (inc/ra8_mstp_regs.h).
const mstp_mipi_csi: u16 = (2 << 8) | 17;

extern fn ra8_mstp_enable(id: u16) u16;
extern fn ra8_mstp_disable(id: u16) u16;
extern fn priv_ra8_mipi_csi_detach_all_handlers() void;

const Csi = struct {
    pub fn read32(_: Csi, off: u16) u32 {
        return @as(*volatile u32, @ptrFromInt(base_addr + off)).*;
    }
    pub fn write32(_: Csi, off: u16, value: u32) void {
        @as(*volatile u32, @ptrFromInt(base_addr + off)).* = value;
    }
    /// RA8_CHECK_NULL_PTR's single log line.
    pub fn err(_: Csi, msg: [*:0]const u8) void {
        common.ra8_log_emit_error(tag, msg);
    }
    /// Same two log lines RA8_RETURN_ON_ERROR emits.
    pub fn errVal(_: Csi, msg: [*:0]const u8, code: u16) void {
        common.ra8_log_emit_error(tag, msg);
        common.ra8_log_emit_error_val(tag, "Error", code);
    }
    pub fn info(_: Csi, msg: [*:0]const u8) void {
        common.ra8_log_emit_info(tag, msg);
    }
    pub fn infoVal(_: Csi, msg: [*:0]const u8, value: u32) void {
        common.ra8_log_emit_info_val(tag, msg, value);
    }
    pub fn mstpEnable(_: Csi) u16 {
        return ra8_mstp_enable(mstp_mipi_csi);
    }
    pub fn mstpDisable(_: Csi) u16 {
        return ra8_mstp_disable(mstp_mipi_csi);
    }
    pub fn detachAll(_: Csi) void {
        priv_ra8_mipi_csi_detach_all_handlers();
    }
};

const csi = Csi{};

export fn ra8_mipi_csi_init(cfg: ?*const lc.Config) u16 {
    return lc.init(csi, cfg);
}

export fn ra8_mipi_csi_deinit() u16 {
    return lc.deinit(csi);
}

export fn ra8_mipi_csi_reset() u16 {
    return lc.reset(csi);
}

export fn ra8_mipi_csi_start_receive() u16 {
    return lc.startReceive(csi);
}

export fn ra8_mipi_csi_stop_receive() u16 {
    return lc.stopReceive(csi);
}
