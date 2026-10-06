//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const st = @import("mipi_dsi_status");

const Fake = struct {
    regs: [0x620 / 4]u32 = @splat(0),
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
    fn set(f: *Fake, off: u16, value: u32) void {
        f.regs[off / 4] = value;
    }
    fn reg(f: *const Fake, off: u16) u32 {
        return f.regs[off / 4];
    }
};

test "get_status reads ISR and rejects null" {
    var f = Fake{};
    f.set(st.off_isr, 0x0010_1011);
    var out: u32 = 0;
    try std.testing.expectEqual(st.ok, st.getStatus(&f, &out));
    try std.testing.expectEqual(@as(u32, 0x0010_1011), out);
    try std.testing.expectEqual(st.null_ptr, st.getStatus(&f, null));
    try std.testing.expectEqual(@as(u8, 1), f.errs);
}

test "link_status_get decodes LINKSR bits" {
    var f = Fake{};
    f.set(st.off_linksr, (1 << 4) | (1 << 13));
    var out: st.LinkStatus = undefined;
    try std.testing.expectEqual(st.ok, st.linkStatusGet(&f, &out));
    try std.testing.expect(!out.sequence_ch0_running);
    try std.testing.expect(out.sequence_ch1_running);
    try std.testing.expect(!out.video_running);
    try std.testing.expect(!out.hs_busy);
    try std.testing.expect(out.lp_busy);
    try std.testing.expectEqual(st.null_ptr, st.linkStatusGet(&f, null));
}

test "clear_status writes only the selected clear registers" {
    var f = Fake{};
    try std.testing.expectEqual(st.ok, st.clearStatus(&f, st.isr_sq1 | st.isr_rcv | st.isr_ppi));
    try std.testing.expectEqual(@as(u32, 0), f.reg(st.off_sqch0scr));
    try std.testing.expectEqual(st.sqch_clear_all, f.reg(st.off_sqch1scr));
    try std.testing.expectEqual(@as(u32, 0), f.reg(st.off_vmscr));
    try std.testing.expectEqual(st.rxsr_clear_all, f.reg(st.off_rxscr));
    try std.testing.expectEqual(@as(u32, 0), f.reg(st.off_ferrscr));
    try std.testing.expectEqual(st.plsr_clear_all, f.reg(st.off_plscr));
}

test "ack_error_get splits report and VC, then write-clears" {
    var f = Fake{};
    f.set(st.off_akepacmsr, 0x000E_ABCD);
    var out: st.AckError = undefined;
    try std.testing.expectEqual(st.ok, st.ackErrorGet(&f, &out));
    try std.testing.expectEqual(@as(u16, 0xABCD), out.error_report);
    try std.testing.expectEqual(@as(u8, 2), out.virtual_channel);
    try std.testing.expectEqual(@as(u32, 0x000E_ABCD), f.reg(st.off_akepscr));
}

test "decodeRx unpacks every RXRSS field" {
    const r = st.decodeRx(0xA5C0_1234 | (0x2A << 16));
    try std.testing.expectEqual(@as(u8, 0x34), r.data[0]);
    try std.testing.expectEqual(@as(u8, 0x12), r.data[1]);
    try std.testing.expectEqual(@as(u8, 0x2A), r.cmd_id);
    try std.testing.expectEqual(@as(u8, 3), r.virtual_channel);
    try std.testing.expect(r.long_packet);
    try std.testing.expect(!r.rx_success);
    try std.testing.expect(r.rx_fatal_error);
    try std.testing.expect(!r.rx_fail);
    try std.testing.expect(!r.rx_packet_data_fail);
    try std.testing.expect(r.rx_correctable_error);
    try std.testing.expect(!r.rx_ack_and_error);
    try std.testing.expect(r.info_overwritten);
}

test "rx_result_get: bad slot, empty slot, valid slot" {
    var f = Fake{};
    var out: st.RxResult = undefined;
    try std.testing.expectEqual(st.invalid_arg, st.rxResultGet(&f, 4, &out));
    try std.testing.expectEqual(st.no_data, st.rxResultGet(&f, 2, &out));
    f.set(st.off_rxrssr, 1 << 2);
    f.set(st.off_rxrss0r + 8, 0x0200_0077);
    try std.testing.expectEqual(st.ok, st.rxResultGet(&f, 2, &out));
    try std.testing.expectEqual(@as(u8, 0x77), out.data[0]);
    try std.testing.expect(out.rx_success);
    try std.testing.expectEqual(@as(u32, 1 << 2), f.reg(st.off_rxrsscr));
    try std.testing.expectEqual(@as(u32, 1 << 2), f.reg(st.off_rxrinfoowscr));
    try std.testing.expectEqual(st.null_ptr, st.rxResultGet(&f, 0, null));
}

test "rx_payload_read copies little-endian bytes capped at 16" {
    var f = Fake{};
    for (0..4) |i| f.set(st.off_rxppd0r + @as(u16, @intCast(i)) * 4, 0x0302_0100 + @as(u32, @intCast(i)) * 0x0404_0404);
    var buf: [20]u8 = @splat(0xEE);
    var n: u16 = 0;
    try std.testing.expectEqual(st.ok, st.rxPayloadRead(&f, &buf, 20, &n));
    try std.testing.expectEqual(@as(u16, 16), n);
    for (0..16) |i| try std.testing.expectEqual(@as(u8, @intCast(i)), buf[i]);
    try std.testing.expectEqual(@as(u8, 0xEE), buf[16]);
    try std.testing.expectEqual(st.ok, st.rxPayloadRead(&f, &buf, 3, &n));
    try std.testing.expectEqual(@as(u16, 3), n);
    try std.testing.expectEqual(st.null_ptr, st.rxPayloadRead(&f, null, 3, &n));
    try std.testing.expectEqual(st.null_ptr, st.rxPayloadRead(&f, &buf, 3, null));
}

test "te_event pending and clear" {
    var f = Fake{};
    var pending = true;
    try std.testing.expectEqual(st.ok, st.teEventPending(&f, &pending));
    try std.testing.expect(!pending);
    f.set(st.off_rxsr, 1 << 15);
    try std.testing.expectEqual(st.ok, st.teEventPending(&f, &pending));
    try std.testing.expect(pending);
    try std.testing.expectEqual(st.ok, st.teEventClear(&f));
    try std.testing.expectEqual(st.rxsr_te_mask, f.reg(st.off_rxscr));
    try std.testing.expectEqual(st.null_ptr, st.teEventPending(&f, null));
}

test "irq_enable sets and clears bits per event, rejects unknown" {
    var f = Fake{};
    const cases = [_][2]u16{
        .{ st.event_seq0, st.off_sqch0ier }, .{ st.event_seq1, st.off_sqch1ier },
        .{ st.event_video, st.off_vmier },   .{ st.event_receive, st.off_rxier },
        .{ st.event_fatal, st.off_ferrier }, .{ st.event_phy, st.off_plier },
    };
    for (cases) |c| {
        f.set(c[1], 0x10);
        try std.testing.expectEqual(st.ok, st.irqEnable(&f, @intCast(c[0]), 0x3, true));
        try std.testing.expectEqual(@as(u32, 0x13), f.reg(c[1]));
        try std.testing.expectEqual(st.ok, st.irqEnable(&f, @intCast(c[0]), 0x11, false));
        try std.testing.expectEqual(@as(u32, 0x02), f.reg(c[1]));
    }
    try std.testing.expectEqual(st.invalid_arg, st.irqEnable(&f, 6, 1, true));
}
