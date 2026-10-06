//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! MIPI CSI-2 interrupt dispatch and VC filtering (RA8FW-627), ported from
//! ra8_mipi_csi_irq.c. Registers are reached through a `csi` ops value
//! (offsets from the CSI base) so host tests can use a fake register file.

pub const ok: u16 = 0;
pub const invalid_arg: u16 = 0x103;

pub const off_mist: u16 = 0x050;
pub const off_dtel: u16 = 0x060;
pub const off_dteh: u16 = 0x064;
pub const off_rxst: u16 = 0x070;
pub const off_rxsc: u16 = 0x074;
pub const off_dlst0: u16 = 0x080;
pub const off_dlsc0: u16 = 0x084;
pub const off_dlst1: u16 = 0x090;
pub const off_dlsc1: u16 = 0x094;
pub const off_vcst0: u16 = 0x100;
pub const off_vcsc0: u16 = 0x104;
pub const off_vcie0: u16 = 0x108;
pub const off_pmst: u16 = 0x200;
pub const off_pmsc: u16 = 0x204;
pub const off_gsst: u16 = 0x284;
pub const off_gssc: u16 = 0x288;

pub const vc_count = 16;
pub const vc_stride: u16 = 0x10;
pub const vc_invalid_idx: u8 = 0xFF;

pub const rxsc_ractdetc: u32 = 0x0002_0000;
pub const dlsc_all: u32 = 0x0003_000F;
pub const mist_dl0s: u32 = 0x1;
pub const mist_dl1s: u32 = 0x2;
pub const mist_vc_mask: u32 = 0xFFFF_0000;
pub const mist_vc_shift = 16;
pub const vcsc_per_vc: u32 = 0x1F01_037F;
pub const vcst_generic_err: u32 = 0x3;
pub const vcst_ecd: u32 = 0x02;
pub const vcst_crc: u32 = 0x04;
pub const vcst_ecc: u32 = 0x20;
pub const pmst_w1c: u32 = 0xFF;
pub const gssc_govc: u32 = 0x10;

pub const dtel_yuv422_8: u32 = 0x4000_0000;
pub const dtel_yuv422_10: u32 = 0x8000_0000;
pub const dteh_rgb888: u32 = 0x10;
pub const dteh_raw8: u32 = 0x400;

/// ra8_mipi_csi_data_format_t values (CSI-2 data types).
pub const format_off: u8 = 0;
pub const format_yuv420: u8 = 0x18;
pub const format_yuv422_8: u8 = 0x1E;
pub const format_yuv422_10: u8 = 0x1F;
pub const format_rgb888: u8 = 0x24;
pub const format_raw8: u8 = 0x2A;
pub const format_raw10: u8 = 0x2B;

/// Mirrors ra8_mipi_csi_error_report_t.
pub const ErrorReport = extern struct {
    vc: u8,
    ecc_corrected: bool,
    ecc_two_bit_error: bool,
    crc_error: bool,
    raw_vcst: u32,
};

pub const EventFn = ?*const fn (ctx: ?*anyopaque, rxst: u32) callconv(.c) void;
pub const LaneFn = ?*const fn (ctx: ?*anyopaque, lane: u8, dlst: u32) callconv(.c) void;
pub const VcFn = ?*const fn (ctx: ?*anyopaque, vc: u8, vcst: u32) callconv(.c) void;
pub const ErrorFn = ?*const fn (ctx: ?*anyopaque, report: *const ErrorReport) callconv(.c) void;

pub fn Handler(comptime F: type) type {
    return struct { f: F = null, ctx: ?*anyopaque = null };
}

pub const State = struct {
    rx: Handler(EventFn) = .{},
    dl: Handler(LaneFn) = .{},
    vc: Handler(VcFn) = .{},
    pm: Handler(EventFn) = .{},
    gst: Handler(EventFn) = .{},
    err: Handler(ErrorFn) = .{},
    vc_format: [vc_count]u8 = @splat(0),
    vcie_saved: [vc_count]u32 = @splat(0),

    /// priv_ra8_mipi_csi_detach_all_handlers: the error handler and the
    /// VC filter state survive, as in the C.
    pub fn detachAll(s: *State) void {
        s.rx = .{};
        s.dl = .{};
        s.vc = .{};
        s.pm = .{};
        s.gst = .{};
    }
};

pub fn vcOff(base: u16, vc: u8) u16 {
    return base + @as(u16, vc) * vc_stride;
}

/// Read a status register, W1C it with `mask`, hand the snapshot on.
fn ackAndCall(csi: anytype, h: Handler(EventFn), st: u16, sc: u16, mask: u32) void {
    const v = csi.read32(st);
    csi.write32(sc, v & mask);
    if (h.f) |f| f(h.ctx, v);
}

pub fn dispatch(s: *const State, csi: anytype) void {
    ackAndCall(csi, s.rx, off_rxst, off_rxsc, rxsc_ractdetc);
}

pub fn dispatchPm(s: *const State, csi: anytype) void {
    ackAndCall(csi, s.pm, off_pmst, off_pmsc, pmst_w1c);
}

pub fn dispatchShortPacket(s: *const State, csi: anytype) void {
    ackAndCall(csi, s.gst, off_gsst, off_gssc, gssc_govc);
}

pub fn dispatchDl(s: *const State, csi: anytype) void {
    const mist = csi.read32(off_mist);
    const dlst0 = csi.read32(off_dlst0);
    const dlst1 = csi.read32(off_dlst1);
    csi.write32(off_dlsc0, dlst0 & dlsc_all);
    csi.write32(off_dlsc1, dlst1 & dlsc_all);
    const f = s.dl.f orelse return;
    if ((mist & mist_dl0s) != 0) f(s.dl.ctx, 0, dlst0);
    if ((mist & mist_dl1s) != 0) f(s.dl.ctx, 1, dlst1);
}

fn reportErrors(s: *const State, vc: u8, vcst: u32) void {
    const f = s.err.f orelse return;
    if ((vcst & (vcst_ecc | vcst_ecd | vcst_crc)) == 0) return;
    const report = ErrorReport{
        .vc = vc,
        .ecc_corrected = (vcst & vcst_ecc) != 0,
        .ecc_two_bit_error = (vcst & vcst_ecd) != 0,
        .crc_error = (vcst & vcst_crc) != 0,
        .raw_vcst = vcst,
    };
    f(s.err.ctx, &report);
}

/// Per-VC events, then one trailing vc=0xFF call carrying the OR of the
/// generic (non-VC-specific) error bits across every flagged VC.
pub fn dispatchVc(s: *const State, csi: anytype) void {
    const flags = (csi.read32(off_mist) & mist_vc_mask) >> mist_vc_shift;
    var generic: u32 = 0;
    var vc: u8 = 0;
    while (vc < vc_count) : (vc += 1) {
        if ((flags & (@as(u32, 1) << @intCast(vc))) == 0) continue;
        const vcst = csi.read32(vcOff(off_vcst0, vc));
        csi.write32(vcOff(off_vcsc0, vc), vcst & vcsc_per_vc);
        generic |= vcst & vcst_generic_err;
        if (s.vc.f) |f| f(s.vc.ctx, vc, vcst & ~vcst_generic_err);
        reportErrors(s, vc, vcst);
    }
    if (s.vc.f) |f| f(s.vc.ctx, vc_invalid_idx, generic);
}

pub const FilterBit = struct { high: bool, bit: u32 };

pub fn formatToBit(format: u8) ?FilterBit {
    return switch (format) {
        format_off => .{ .high = false, .bit = 0 },
        format_yuv422_8 => .{ .high = false, .bit = dtel_yuv422_8 },
        format_yuv422_10 => .{ .high = false, .bit = dtel_yuv422_10 },
        format_rgb888 => .{ .high = true, .bit = dteh_rgb888 },
        format_raw8 => .{ .high = true, .bit = dteh_raw8 },
        else => null,
    };
}

fn recomputeDtFilter(s: *const State, csi: anytype) void {
    var low: u32 = 0;
    var high: u32 = 0;
    for (s.vc_format) |fmt| {
        const fb = formatToBit(fmt) orelse FilterBit{ .high = false, .bit = 0 };
        if (fb.high) high |= fb.bit else low |= fb.bit;
    }
    csi.write32(off_dtel, low);
    csi.write32(off_dteh, high);
}

pub fn setDataFormat(s: *State, csi: anytype, vc: u8, format: u8) u16 {
    if (vc >= vc_count) return invalid_arg;
    if (formatToBit(format) == null) return invalid_arg;
    s.vc_format[vc] = format;
    recomputeDtFilter(s, csi);
    return ok;
}

/// Masked-off VCs park their nonzero VCIE value and go quiet; VCs back in
/// the mask get the parked value restored.
pub fn setVirtualChannels(s: *State, csi: anytype, vc_mask: u16) u16 {
    if (vc_mask == 0) {
        csi.err("set_virtual_channels: empty mask rejected");
        return invalid_arg;
    }
    var vc: u8 = 0;
    while (vc < vc_count) : (vc += 1) {
        const off = vcOff(off_vcie0, vc);
        if ((vc_mask & (@as(u16, 1) << @intCast(vc))) != 0) {
            if (s.vcie_saved[vc] != 0) {
                csi.write32(off, s.vcie_saved[vc]);
                s.vcie_saved[vc] = 0;
            }
        } else {
            const cur = csi.read32(off);
            if (cur != 0) s.vcie_saved[vc] = cur;
            csi.write32(off, 0);
        }
    }
    return ok;
}
