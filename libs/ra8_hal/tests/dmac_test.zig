//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const dmac = @import("dmac");

const Fake = struct {
    regs: [dmac.channel_count]dmac.ChannelRegs = [_]dmac.ChannelRegs{.{}} ** dmac.channel_count,
    dmast_reg: u8 = 0,
    mstp_on: u8 = 0,
    mstp_off: u8 = 0,
    mstp_err: u16 = dmac.ok,
    nulls: u8 = 0,
    fails: u8 = 0,
    infos: u8 = 0,

    pub fn channel(f: *Fake, ch: u8) ?*volatile dmac.ChannelRegs {
        if (ch >= dmac.channel_count) return null;
        return &f.regs[ch];
    }
    pub fn dmast(f: *Fake) *volatile u8 {
        return &f.dmast_reg;
    }
    pub fn mstpEnable(f: *Fake, id: u16) u16 {
        std.debug.assert(id == dmac.mstp_dmac0_dtc0);
        f.mstp_on += 1;
        return f.mstp_err;
    }
    pub fn mstpDisable(f: *Fake, id: u16) u16 {
        std.debug.assert(id == dmac.mstp_dmac0_dtc0);
        f.mstp_off += 1;
        return dmac.ok;
    }
    pub fn nullPtr(f: *Fake, _: [*:0]const u8) u16 {
        f.nulls += 1;
        return dmac.null_ptr;
    }
    pub fn fail(f: *Fake, _: [*:0]const u8, _: u16) void {
        f.fails += 1;
    }
    pub fn infoVal(f: *Fake, _: [*:0]const u8, _: u32) void {
        f.infos += 1;
    }
};

const expectEqual = std.testing.expectEqual;

test "priv helpers match the C predicates" {
    try std.testing.expect(dmac.modeDisablesDts(0, 3, 0));
    try std.testing.expect(dmac.modeDisablesDts(0, 3, 3));
    try std.testing.expect(!dmac.modeDisablesDts(0, 3, 1));
    try std.testing.expect(dmac.dmintExtraIrq(true, 3, 1));
    try std.testing.expect(!dmac.dmintExtraIrq(true, 3, 3));
    try std.testing.expect(!dmac.dmintExtraIrq(false, 3, 1));
}

test "encoders: DMTMD, DMAMD, DMINT, DMCRA" {
    const rep = dmac.Config{ .width = 2, .mode = dmac.mode_repeat, .repeat_area = dmac.area_src, .count = 0x1234, .irq_each = true, .enable_dtie = true };
    // SZ=10b<<8, MD=01b<<14, DTS=01b<<12.
    try expectEqual(@as(u16, 0x0200 | 0x4000 | 0x1000), dmac.dmtmdValue(&rep));
    try expectEqual(dmac.dmint_dtie | dmac.dmint_rptie | dmac.dmint_esie, dmac.dmintValue(&rep));
    // Count in both halves, each masked to 10 bits.
    try expectEqual(@as(u32, 0x0234_0234), dmac.dmcraValue(&rep));
    const norm = dmac.Config{ .width = 0, .mode = dmac.mode_normal, .repeat_area = dmac.area_dest, .count = 0x1234 };
    // Normal mode forces DTS=10b and leaves DMCRA unmasked.
    try expectEqual(@as(u16, 0x2000), dmac.dmtmdValue(&norm));
    try expectEqual(@as(u32, 0x1234), dmac.dmcraValue(&norm));
    try expectEqual(@as(u8, 0), dmac.dmintValue(&norm));
    try expectEqual(@as(u16, 0x8080), dmac.dmamdValue(true, true));
    try expectEqual(@as(u16, 0x8000), dmac.dmamdValue(true, false));
}

test "start programs the channel, sets DMST and DTE" {
    var f = Fake{};
    f.regs[3].dmofr = 0xFFFF;
    const cfg = dmac.Config{ .src = 0x2000_0000, .dst = 0x2000_1000, .count = 16, .width = 1, .src_inc = true, .dst_inc = true, .mode = dmac.mode_block, .block_count = 4, .enable_dtie = true };
    try expectEqual(dmac.ok, dmac.start(&f, 3, &cfg));
    const r = f.regs[3];
    try expectEqual(@as(u32, 0x2000_0000), r.dmsar);
    try expectEqual(@as(u32, 0x2000_1000), r.dmdar);
    try expectEqual(@as(u32, 0x0010_0010), r.dmcra);
    try expectEqual(@as(u32, 0x0004_0004), r.dmcrb);
    try expectEqual(@as(u32, 0), r.dmofr);
    try expectEqual(dmac.dmtmdValue(&cfg), r.dmtmd);
    try expectEqual(@as(u16, 0x8080), r.dmamd);
    // irq_each is off, so only DTIE is set.
    try expectEqual(dmac.dmint_dtie, r.dmint);
    try expectEqual(dmac.dmcnt_dte, r.dmcnt);
    try expectEqual(dmac.dmast_dmst, f.dmast_reg);
    try expectEqual(@as(u8, 1), f.mstp_on);
    try expectEqual(@as(u8, 1), f.infos);
}

test "start normal mode zeroes DMCRB" {
    var f = Fake{};
    f.regs[0].dmcrb = 0xDEAD;
    const cfg = dmac.Config{ .count = 5, .block_count = 9 };
    try expectEqual(dmac.ok, dmac.start(&f, 0, &cfg));
    try expectEqual(@as(u32, 0), f.regs[0].dmcrb);
    try expectEqual(@as(u32, 5), f.regs[0].dmcra);
}

test "start rejects null, bad cfg and bad channel before touching MSTP" {
    var f = Fake{};
    try expectEqual(dmac.null_ptr, dmac.start(&f, 0, null));
    try expectEqual(@as(u8, 1), f.nulls);
    try expectEqual(dmac.invalid_arg, dmac.start(&f, 0, &dmac.Config{ .width = 3 }));
    try expectEqual(dmac.invalid_arg, dmac.start(&f, 0, &dmac.Config{ .mode = 4 }));
    try expectEqual(dmac.out_of_range, dmac.start(&f, 8, &dmac.Config{}));
    try expectEqual(@as(u8, 0), f.mstp_on);
}

test "start returns the MSTP error and leaves the channel alone" {
    var f = Fake{ .mstp_err = 0x201 };
    try expectEqual(@as(u16, 0x201), dmac.start(&f, 1, &dmac.Config{ .count = 7 }));
    try expectEqual(@as(u8, 1), f.fails);
    try expectEqual(@as(u32, 0), f.regs[1].dmcra);
    try expectEqual(@as(u8, 0), f.dmast_reg);
}

test "stop clears DTE and drops the MSTP reference" {
    var f = Fake{};
    f.regs[2].dmcnt = dmac.dmcnt_dte;
    try expectEqual(dmac.ok, dmac.stop(&f, 2));
    try expectEqual(@as(u8, 0), f.regs[2].dmcnt);
    try expectEqual(@as(u8, 1), f.mstp_off);
    try expectEqual(dmac.out_of_range, dmac.stop(&f, 9));
    try expectEqual(@as(u8, 1), f.mstp_off);
}

test "start_repeat and start_block force the mode on a copy" {
    var f = Fake{};
    const cfg = dmac.Config{ .count = 3, .mode = dmac.mode_normal, .block_count = 2 };
    try expectEqual(dmac.ok, dmac.startWithMode(&f, 4, &cfg, dmac.mode_repeat));
    try expectEqual(dmac.mode_normal, cfg.mode);
    try expectEqual(@as(u16, 1) << 14, f.regs[4].dmtmd & 0xC000);
    try expectEqual(dmac.ok, dmac.startBlock(&f, 5, &cfg));
    try expectEqual(@as(u16, 2) << 14, f.regs[5].dmtmd & 0xC000);
    try expectEqual(dmac.invalid_arg, dmac.startBlock(&f, 5, &dmac.Config{ .block_count = 0 }));
    try expectEqual(dmac.null_ptr, dmac.startBlock(&f, 5, null));
    try expectEqual(dmac.null_ptr, dmac.startWithMode(&f, 5, null, dmac.mode_repeat));
}

test "set_address_mode rewrites SM/DM only" {
    var f = Fake{};
    f.regs[6].dmamd = 0xFFFF;
    try expectEqual(dmac.ok, dmac.setAddressMode(&f, 6, 3, 0));
    try expectEqual(@as(u16, 0xFF3F), f.regs[6].dmamd);
    try expectEqual(dmac.invalid_arg, dmac.setAddressMode(&f, 6, 4, 0));
    try expectEqual(dmac.invalid_arg, dmac.setAddressMode(&f, 6, 0, 4));
    try expectEqual(dmac.out_of_range, dmac.setAddressMode(&f, 8, 0, 0));
}

test "software trigger, is_active and wait_idle" {
    var f = Fake{};
    try expectEqual(dmac.ok, dmac.softwareTrigger(&f, 7));
    try expectEqual(dmac.dmreq_swreq, f.regs[7].dmreq);
    try expectEqual(dmac.out_of_range, dmac.softwareTrigger(&f, 8));
    var active = true;
    try expectEqual(dmac.ok, dmac.isActive(&f, 7, &active));
    try std.testing.expect(!active);
    f.regs[7].dmsts = dmac.dmsts_act;
    try expectEqual(dmac.ok, dmac.isActive(&f, 7, &active));
    try std.testing.expect(active);
    try expectEqual(dmac.null_ptr, dmac.isActive(&f, 7, null));
    try expectEqual(dmac.out_of_range, dmac.isActive(&f, 8, &active));
    try expectEqual(dmac.hw_timeout, dmac.waitIdle(&f, 7, 10));
    try expectEqual(dmac.hw_timeout, dmac.waitIdle(&f, 7, 0));
    f.regs[7].dmsts = 0x7F;
    try expectEqual(dmac.ok, dmac.waitIdle(&f, 7, 1));
    try expectEqual(dmac.out_of_range, dmac.waitIdle(&f, 8, 1));
}

var hits: [2]u32 = .{ 0, 0 };

fn onFull(ctx: ?*anyopaque) callconv(.c) void {
    _ = ctx;
    hits[0] += 1;
}

fn onHalf(ctx: ?*anyopaque) callconv(.c) void {
    const n: *u32 = @ptrCast(@alignCast(ctx.?));
    n.* += 1;
    hits[1] += 1;
}

test "callback slots attach and dispatch per channel" {
    var s = dmac.Slots{};
    var counter: u32 = 0;
    hits = .{ 0, 0 };
    try expectEqual(dmac.ok, s.attach(1, false, &onFull, null));
    try expectEqual(dmac.ok, s.attach(1, true, &onHalf, &counter));
    s.dispatch(1, false);
    s.dispatch(1, true);
    s.dispatch(2, false);
    s.dispatch(8, true);
    try expectEqual(@as(u32, 1), hits[0]);
    try expectEqual(@as(u32, 1), hits[1]);
    try expectEqual(@as(u32, 1), counter);
    try expectEqual(dmac.out_of_range, s.attach(8, false, &onFull, null));
    try expectEqual(dmac.ok, s.attach(1, false, null, null));
    s.dispatch(1, false);
    try expectEqual(@as(u32, 1), hits[0]);
}

test "channel register block layout" {
    try expectEqual(@as(usize, 0x40), @sizeOf(dmac.ChannelRegs));
    try expectEqual(@as(usize, 0x20), @offsetOf(dmac.ChannelRegs, "dmsrr"));
}
