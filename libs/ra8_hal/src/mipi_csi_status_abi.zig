//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the lane / VC / PM / short-packet / stop functions declared in
//! ra8_mipi_csi_api.h and ra8_mipi_csi_isr.h (RA8FW-632), replacing the tail
//! of ra8_mipi_csi.c.

const common = @import("abi_common.zig");
const st = @import("internal/mipi_csi_status.zig");

const tag = "MIPI_CSI";
const base_addr: usize = 0x40347000;
/// `ra8_mstp_t` k_ra8_mstp_mipi_csi: (k_ra8_mstp_reg_c << 8) | 17 (inc/ra8_mstp_regs.h).
const mstp_mipi_csi: u16 = (2 << 8) | 17;

extern fn ra8_mstp_enable(id: u16) u16;
extern fn ra8_mstp_disable(id: u16) u16;

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

const csi = Csi{};

export fn ra8_mipi_csi_dl_get_status(lane: u8, out_mask: ?*u32) u16 {
    return st.getIndexed(csi, .lane, st.off_dlst0, lane, out_mask);
}

export fn ra8_mipi_csi_dl_clear_status(lane: u8, mask: u32) u16 {
    return st.writeIndexed(csi, .lane, st.off_dlsc0, lane, mask & st.dlsc_all);
}

export fn ra8_mipi_csi_dl_set_irq_enable(lane: u8, mask: u32) u16 {
    return st.writeIndexed(csi, .lane, st.off_dlie0, lane, mask);
}

export fn ra8_mipi_csi_vc_get_status(vc: u8, out_mask: ?*u32) u16 {
    return st.getIndexed(csi, .vc, st.off_vcst0, vc, out_mask);
}

export fn ra8_mipi_csi_vc_clear_status(vc: u8, mask: u32) u16 {
    return st.writeIndexed(csi, .vc, st.off_vcsc0, vc, mask);
}

export fn ra8_mipi_csi_vc_set_irq_enable(vc: u8, mask: u32) u16 {
    return st.writeIndexed(csi, .vc, st.off_vcie0, vc, mask);
}

export fn ra8_mipi_csi_pm_get_status(out_mask: ?*u32) u16 {
    return st.getReg(csi, st.off_pmst, out_mask);
}

export fn ra8_mipi_csi_pm_clear_status(mask: u32) u16 {
    csi.write32(st.off_pmsc, mask & st.pm_all);
    return common.k_ra8_ok;
}

export fn ra8_mipi_csi_pm_set_irq_enable(mask: u32) u16 {
    csi.write32(st.off_pmie, mask & st.pm_all);
    return common.k_ra8_ok;
}

export fn ra8_mipi_csi_short_packet_configure(threshold: u8, store_enable: bool) u16 {
    return st.configureShortPacket(csi, threshold, store_enable);
}

export fn ra8_mipi_csi_short_packet_set_irq_enable(mask: u32) u16 {
    csi.write32(st.off_gsie, mask & st.gsie_all);
    return common.k_ra8_ok;
}

export fn ra8_mipi_csi_short_packet_get_status(out_mask: ?*u32) u16 {
    return st.getReg(csi, st.off_gsst, out_mask);
}

export fn ra8_mipi_csi_short_packet_clear_status(mask: u32) u16 {
    csi.write32(st.off_gssc, mask & st.gssc_govc);
    return common.k_ra8_ok;
}

export fn ra8_mipi_csi_read_short_packet(out: ?*st.ShortPacket) u16 {
    return st.readShortPacket(csi, out);
}

export fn ra8_mipi_csi_short_packet_clear_fifo() u16 {
    return st.clearFifo(csi);
}

export fn ra8_mipi_csi_short_packet_re_enable_store() u16 {
    csi.write32(st.off_gsiu, st.gsiu_gfen);
    return common.k_ra8_ok;
}

export fn ra8_mipi_csi_enter_stop() u16 {
    return ra8_mstp_disable(mstp_mipi_csi);
}

export fn ra8_mipi_csi_exit_stop() u16 {
    return ra8_mstp_enable(mstp_mipi_csi);
}
