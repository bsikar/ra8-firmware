//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const irq = @import("mipi_csi_irq");

const Fake = struct {
    regs: [0x300 / 4]u32 = [_]u32{0} ** (0x300 / 4),
    errs: u8 = 0,

    pub fn read32(f: *Fake, off: u16) u32 {
        return f.regs[off / 4];
    }
    pub fn write32(f: *Fake, off: u16, value: u32) void {
        f.regs[off / 4] = value;
    }
    pub fn err(f: *Fake, _: [*:0]const u8) void {
        f.errs += 1;
    }
    fn at(f: *const Fake, off: u16) u32 {
        return f.regs[off / 4];
    }
    fn set(f: *Fake, off: u16, value: u32) void {
        f.regs[off / 4] = value;
    }
};

const Rec = struct {
    calls: u8 = 0,
    ids: [20]u8 = [_]u8{0} ** 20,
    vals: [20]u32 = [_]u32{0} ** 20,
    reports: u8 = 0,
    last: irq.ErrorReport = .{ .vc = 0, .ecc_corrected = false, .ecc_two_bit_error = false, .crc_error = false, .raw_vcst = 0 },
};

fn onEvent(ctx: ?*anyopaque, v: u32) callconv(.c) void {
    const r: *Rec = @ptrCast(@alignCast(ctx.?));
    r.vals[r.calls] = v;
    r.calls += 1;
}

fn onPair(ctx: ?*anyopaque, id: u8, v: u32) callconv(.c) void {
    const r: *Rec = @ptrCast(@alignCast(ctx.?));
    r.ids[r.calls] = id;
    r.vals[r.calls] = v;
    r.calls += 1;
}

fn onError(ctx: ?*anyopaque, report: *const irq.ErrorReport) callconv(.c) void {
    const r: *Rec = @ptrCast(@alignCast(ctx.?));
    r.last = report.*;
    r.reports += 1;
}

test "dispatch clears only RXDET and passes the RXST snapshot" {
    var f = Fake{};
    var r = Rec{};
    var s = irq.State{ .rx = .{ .f = onEvent, .ctx = &r } };
    f.set(irq.off_rxst, 0x0003_0001);
    irq.dispatch(&s, &f);
    try std.testing.expectEqual(@as(u32, 0x0002_0000), f.at(irq.off_rxsc));
    try std.testing.expectEqual(@as(u32, 0x0003_0001), r.vals[0]);
}

test "pm and short-packet dispatch W1C their own masks" {
    var f = Fake{};
    var r = Rec{};
    var s = irq.State{ .pm = .{ .f = onEvent, .ctx = &r }, .gst = .{ .f = onEvent, .ctx = &r } };
    f.set(irq.off_pmst, 0x1FF);
    f.set(irq.off_gsst, 0x31);
    irq.dispatchPm(&s, &f);
    irq.dispatchShortPacket(&s, &f);
    try std.testing.expectEqual(@as(u32, 0xFF), f.at(irq.off_pmsc));
    try std.testing.expectEqual(@as(u32, 0x10), f.at(irq.off_gssc));
    try std.testing.expectEqual(@as(u32, 0x1FF), r.vals[0]);
    try std.testing.expectEqual(@as(u32, 0x31), r.vals[1]);
}

test "dispatch without a handler still clears status" {
    var f = Fake{};
    const s = irq.State{};
    f.set(irq.off_rxst, 0xFFFF_FFFF);
    f.set(irq.off_dlst1, 0xFF);
    irq.dispatch(&s, &f);
    irq.dispatchDl(&s, &f);
    try std.testing.expectEqual(@as(u32, 0x0002_0000), f.at(irq.off_rxsc));
    try std.testing.expectEqual(@as(u32, 0x0F), f.at(irq.off_dlsc1));
}

test "dispatch_dl fires per lane flagged in MIST" {
    var f = Fake{};
    var r = Rec{};
    const s = irq.State{ .dl = .{ .f = onPair, .ctx = &r } };
    f.set(irq.off_mist, irq.mist_dl1s);
    f.set(irq.off_dlst0, 0x0004_0001);
    f.set(irq.off_dlst1, 0x0003_0002);
    irq.dispatchDl(&s, &f);
    try std.testing.expectEqual(@as(u32, 0x0000_0001), f.at(irq.off_dlsc0));
    try std.testing.expectEqual(@as(u32, 0x0003_0002), f.at(irq.off_dlsc1));
    try std.testing.expectEqual(@as(u8, 1), r.calls);
    try std.testing.expectEqual(@as(u8, 1), r.ids[0]);
    try std.testing.expectEqual(@as(u32, 0x0003_0002), r.vals[0]);
}

test "dispatch_vc splits per-VC and generic bits and reports errors" {
    var f = Fake{};
    var r = Rec{};
    var e = Rec{};
    const s = irq.State{ .vc = .{ .f = onPair, .ctx = &r }, .err = .{ .f = onError, .ctx = &e } };
    f.set(irq.off_mist, (1 << 16) | (1 << 31));
    f.set(irq.vcOff(irq.off_vcst0, 0), 0x0000_0101);
    f.set(irq.vcOff(irq.off_vcst0, 15), 0x8000_0026);
    irq.dispatchVc(&s, &f);
    try std.testing.expectEqual(@as(u32, 0x101), f.at(irq.vcOff(irq.off_vcsc0, 0)));
    try std.testing.expectEqual(@as(u32, 0x26), f.at(irq.vcOff(irq.off_vcsc0, 15)));
    try std.testing.expectEqual(@as(u8, 3), r.calls);
    try std.testing.expectEqual(@as(u32, 0x100), r.vals[0]);
    try std.testing.expectEqual(@as(u8, 15), r.ids[1]);
    try std.testing.expectEqual(@as(u32, 0x8000_0024), r.vals[1]);
    try std.testing.expectEqual(irq.vc_invalid_idx, r.ids[2]);
    try std.testing.expectEqual(@as(u32, 0x3), r.vals[2]);
    try std.testing.expectEqual(@as(u8, 1), e.reports);
    try std.testing.expectEqual(@as(u8, 15), e.last.vc);
    try std.testing.expect(e.last.ecc_corrected and e.last.ecc_two_bit_error and e.last.crc_error);
    try std.testing.expectEqual(@as(u32, 0x8000_0026), e.last.raw_vcst);
}

test "dispatch_vc with no flagged VC still sends the generic call" {
    var f = Fake{};
    var r = Rec{};
    const s = irq.State{ .vc = .{ .f = onPair, .ctx = &r } };
    irq.dispatchVc(&s, &f);
    try std.testing.expectEqual(@as(u8, 1), r.calls);
    try std.testing.expectEqual(irq.vc_invalid_idx, r.ids[0]);
    try std.testing.expectEqual(@as(u32, 0), r.vals[0]);
}

test "set_data_format rewrites DTEL and DTEH from every VC" {
    var f = Fake{};
    var s = irq.State{};
    try std.testing.expectEqual(irq.ok, irq.setDataFormat(&s, &f, 0, irq.format_yuv422_8));
    try std.testing.expectEqual(irq.ok, irq.setDataFormat(&s, &f, 3, irq.format_raw8));
    try std.testing.expectEqual(irq.ok, irq.setDataFormat(&s, &f, 7, irq.format_rgb888));
    try std.testing.expectEqual(irq.dtel_yuv422_8, f.at(irq.off_dtel));
    try std.testing.expectEqual(irq.dteh_raw8 | irq.dteh_rgb888, f.at(irq.off_dteh));
    try std.testing.expectEqual(irq.ok, irq.setDataFormat(&s, &f, 3, irq.format_off));
    try std.testing.expectEqual(irq.dteh_rgb888, f.at(irq.off_dteh));
}

test "set_data_format rejects a bad VC and unsupported formats" {
    var f = Fake{};
    var s = irq.State{};
    try std.testing.expectEqual(irq.invalid_arg, irq.setDataFormat(&s, &f, 16, irq.format_raw8));
    try std.testing.expectEqual(irq.invalid_arg, irq.setDataFormat(&s, &f, 0, irq.format_raw10));
    try std.testing.expectEqual(irq.invalid_arg, irq.setDataFormat(&s, &f, 0, irq.format_yuv420));
    try std.testing.expectEqual(irq.invalid_arg, irq.setDataFormat(&s, &f, 0, 0x77));
    try std.testing.expectEqual(@as(u8, 0), s.vc_format[0]);
}

test "set_virtual_channels parks and restores VCIE" {
    var f = Fake{};
    var s = irq.State{};
    try std.testing.expectEqual(irq.invalid_arg, irq.setVirtualChannels(&s, &f, 0));
    try std.testing.expectEqual(@as(u8, 1), f.errs);
    f.set(irq.vcOff(irq.off_vcie0, 2), 0x55);
    try std.testing.expectEqual(irq.ok, irq.setVirtualChannels(&s, &f, 0x0001));
    try std.testing.expectEqual(@as(u32, 0), f.at(irq.vcOff(irq.off_vcie0, 2)));
    try std.testing.expectEqual(@as(u32, 0x55), s.vcie_saved[2]);
    try std.testing.expectEqual(irq.ok, irq.setVirtualChannels(&s, &f, 0xFFFF));
    try std.testing.expectEqual(@as(u32, 0x55), f.at(irq.vcOff(irq.off_vcie0, 2)));
    try std.testing.expectEqual(@as(u32, 0), s.vcie_saved[2]);
}

test "detachAll drops event handlers but keeps the error handler" {
    var r = Rec{};
    var s = irq.State{
        .rx = .{ .f = onEvent, .ctx = &r },
        .vc = .{ .f = onPair, .ctx = &r },
        .err = .{ .f = onError, .ctx = &r },
    };
    s.vc_format[1] = irq.format_raw8;
    s.detachAll();
    try std.testing.expect(s.rx.f == null and s.rx.ctx == null and s.vc.f == null);
    try std.testing.expect(s.err.f != null);
    try std.testing.expectEqual(irq.format_raw8, s.vc_format[1]);
}
