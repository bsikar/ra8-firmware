//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/cgc_eswclk.zig.

const std = @import("std");
const e = @import("cgc_eswclk");

/// R_SYSTEM bytes with the two handshakes modelled: CKSRDY follows CKSREQ
/// and clearing PDDE powers the domain (PDCSF/PDPGSF clear) unless stuck.
const Regs = struct {
    mem: [0x400]u8 = [_]u8{0} ** 0x400,
    prcr: [8]u16 = undefined,
    np: usize = 0,
    ck: [8]u8 = undefined,
    nck: usize = 0,
    stuck_ckcr: usize = 0,
    stuck_pd: u8 = 0,
    pd_writes: usize = 0,

    pub fn read8(self: *Regs, off: usize) u8 {
        return self.mem[off];
    }
    pub fn write8(self: *Regs, off: usize, value: u8) void {
        var v = value;
        if (off == e.off_eswckcr or off == e.off_eswpckcr) {
            if (off == e.off_eswckcr) {
                self.ck[self.nck] = value;
                self.nck += 1;
            }
            if (off != self.stuck_ckcr) v = if (v & e.sreq != 0) v | e.srdy else v & ~e.srdy;
        }
        if (off == e.off_pdctreswm) {
            self.pd_writes += 1;
            if (v & e.pdde == 0) v &= ~((e.pdcsf | e.pdpgsf) & ~self.stuck_pd);
        }
        self.mem[off] = v;
    }
    pub fn write16(self: *Regs, off: usize, value: u16) void {
        std.debug.assert(off == e.off_prcr);
        self.prcr[self.np] = value;
        self.np += 1;
    }
};

const Ops = struct {
    hoco_err: u16 = e.ok,
    mstp_err: u16 = e.ok,
    mstp_id: u16 = 0,
    infos: usize = 0,
    last_err: ?[]const u8 = null,

    pub fn hoco(self: *Ops) u16 {
        return self.hoco_err;
    }
    pub fn mstpEnable(self: *Ops, id: u16) u16 {
        self.mstp_id = id;
        return self.mstp_err;
    }
    pub fn info(self: *Ops, _: [*:0]const u8) void {
        self.infos += 1;
    }
    pub fn err(self: *Ops, msg: [*:0]const u8) void {
        self.last_err = std.mem.span(msg);
    }
};

/// The reset state: domain gated (PDCSF=0, PDPGSF=1, PDDE=1).
fn gated() Regs {
    var r = Regs{};
    r.mem[e.off_pdctreswm] = e.pdpgsf | e.pdde;
    return r;
}

test "init powers the domain, releases MSTPC28 and switches both clocks" {
    var r = gated();
    var o = Ops{};
    var hz: u32 = 0;
    try std.testing.expectEqual(e.ok, e.init(&r, &o, &hz));
    try std.testing.expectEqual(e.eswclk_hz, hz);
    try std.testing.expectEqual(e.sel_pll1p, r.mem[e.off_eswckcr]);
    try std.testing.expectEqual(e.sel_pll1p, r.mem[e.off_eswpckcr]);
    try std.testing.expectEqual(e.div4, r.mem[e.off_eswckdivcr]);
    try std.testing.expectEqual(e.div2, r.mem[e.off_eswpckdivcr]);
    try std.testing.expectEqual(@as(u8, 0), r.mem[e.off_pdctreswm]);
    try std.testing.expectEqual(e.mstp_ethphyclk, o.mstp_id);
    try std.testing.expectEqual(@as(usize, 2), o.infos);
    try std.testing.expectEqual(@as(?[]const u8, null), o.last_err);
    const want = [_]u16{ 0xA502, 0xA500, 0xA501, 0xA500 };
    try std.testing.expectEqualSlices(u16, &want, r.prcr[0..r.np]);
}

test "the clock switch follows the FSP SREQ/SRDY write order" {
    var r = Regs{};
    r.mem[e.off_eswckcr] = 0x01;
    try std.testing.expectEqual(e.ok, e.switchToPll1p(&r, e.off_eswckcr, e.off_eswckdivcr, e.div4));
    const want = [_]u8{ 0x01 | e.sreq, e.sel_pll1p | e.sreq | e.srdy, e.sel_pll1p | e.srdy };
    try std.testing.expectEqualSlices(u8, &want, r.ck[0..r.nck]);
}

test "a HOCO failure is returned before any register is touched" {
    var r = gated();
    var o = Ops{ .hoco_err = e.hw_timeout };
    var hz: u32 = 7;
    try std.testing.expectEqual(e.hw_timeout, e.init(&r, &o, &hz));
    try std.testing.expectEqual(@as(u32, 7), hz);
    try std.testing.expectEqual(@as(usize, 0), r.np);
    try std.testing.expectEqualStrings("eswclk: HOCO stabilization timeout", o.last_err.?);
}

test "a domain that is already on is not written" {
    var r = Regs{};
    var o = Ops{};
    try std.testing.expectEqual(e.ok, e.powerOnDomain(&r, &o));
    try std.testing.expectEqual(@as(usize, 0), r.pd_writes);
}

test "a stuck PDCSF or PDPGSF times out and names the flag" {
    inline for (.{ .{ e.pdcsf, "eswclk: PDCSF stuck" }, .{ e.pdpgsf, "eswclk: PDPGSF stuck" } }) |c| {
        var r = gated();
        r.mem[e.off_pdctreswm] |= c[0];
        r.stuck_pd = c[0];
        var o = Ops{};
        var hz: u32 = 0;
        try std.testing.expectEqual(e.hw_timeout, e.init(&r, &o, &hz));
        try std.testing.expectEqual(@as(u16, 0), o.mstp_id);
        try std.testing.expectEqualStrings(c[1], o.last_err.?);
    }
}

test "the PDCSF check skips power-on when the domain reports busy" {
    var r = Regs{};
    r.mem[e.off_pdctreswm] = e.pdcsf | e.pdpgsf | e.pdde;
    r.stuck_pd = e.pdcsf;
    var o = Ops{};
    try std.testing.expectEqual(e.hw_timeout, e.powerOnDomain(&r, &o));
    try std.testing.expectEqual(@as(usize, 0), r.pd_writes);
}

test "an MSTP failure is returned and logged" {
    var r = gated();
    var o = Ops{ .mstp_err = 0x104 };
    var hz: u32 = 0;
    try std.testing.expectEqual(@as(u16, 0x104), e.init(&r, &o, &hz));
    try std.testing.expectEqual(@as(u32, 0), hz);
    try std.testing.expectEqualStrings("eswclk: ethphyclk MSTP release failed", o.last_err.?);
}

test "an ESWCKCR handshake timeout re-locks PRCR and leaves ESWPCKCR alone" {
    var r = gated();
    r.stuck_ckcr = e.off_eswckcr;
    var o = Ops{};
    var hz: u32 = 0;
    try std.testing.expectEqual(e.hw_timeout, e.init(&r, &o, &hz));
    try std.testing.expectEqual(@as(u32, 0), hz);
    try std.testing.expectEqual(@as(u8, 0), r.mem[e.off_eswpckcr]);
    try std.testing.expectEqual(@as(u16, 0xA500), r.prcr[r.np - 1]);
    try std.testing.expectEqualStrings("eswclk: ESWCKCR handshake timeout", o.last_err.?);
}

test "an ESWPCKCR handshake timeout is reported after ESWCKCR switched" {
    var r = gated();
    r.stuck_ckcr = e.off_eswpckcr;
    var o = Ops{};
    var hz: u32 = 0;
    try std.testing.expectEqual(e.hw_timeout, e.init(&r, &o, &hz));
    try std.testing.expectEqual(e.sel_pll1p, r.mem[e.off_eswckcr]);
    try std.testing.expectEqual(@as(u16, 0xA500), r.prcr[r.np - 1]);
    try std.testing.expectEqualStrings("eswclk: ESWPCKCR handshake timeout", o.last_err.?);
}
