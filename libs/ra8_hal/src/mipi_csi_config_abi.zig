//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the MIPI CSI receiver option setters declared in
//! ra8_mipi_csi_api.h (RA8FW-634), replacing that part of ra8_mipi_csi.c.

const common = @import("abi_common.zig");
const cfg = @import("internal/mipi_csi_config.zig");

const tag = "MIPI_CSI";
const base_addr: usize = 0x40347000;

const Csi = struct {
    pub fn read32(_: Csi, off: u16) u32 {
        return @as(*volatile u32, @ptrFromInt(base_addr + off)).*;
    }
    pub fn write32(_: Csi, off: u16, value: u32) void {
        @as(*volatile u32, @ptrFromInt(base_addr + off)).* = value;
    }
    /// Same two log lines RA8_RETURN_ON_ERROR emits.
    pub fn errVal(_: Csi, msg: [*:0]const u8, code: u16) void {
        common.ra8_log_emit_error(tag, msg);
        common.ra8_log_emit_error_val(tag, "Error", code);
    }
};

const csi = Csi{};

export fn ra8_mipi_csi_set_data_type_filter(low_mask: u32, high_mask: u32) u16 {
    return cfg.setDataTypeFilter(csi, low_mask, high_mask);
}

export fn ra8_mipi_csi_set_ecc_mode(eccv13: bool, lfsren: bool) u16 {
    return cfg.setEccMode(csi, eccv13, lfsren);
}

export fn ra8_mipi_csi_set_frame_error_mode(zlmd: bool, edmd: bool, rvmd: bool) u16 {
    return cfg.setFrameErrorMode(csi, zlmd, edmd, rvmd);
}

export fn ra8_mipi_csi_set_epd(enable: bool, option_2: bool, long_spacer: u16, short_spacer: u16) u16 {
    return cfg.setEpd(csi, enable, option_2, long_spacer, short_spacer);
}

export fn ra8_mipi_csi_set_lrte(vlsien: u8, eotp_enable: bool) u16 {
    return cfg.setLrte(csi, vlsien, eotp_enable);
}
