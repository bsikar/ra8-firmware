//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! PVD channel logic (RA8FW-797), ported from ra8_lvd.c. PVD1/2 are the m
//! channels (IRQ/NMI capable), PVD4/5 the n channels (reset only). Addresses
//! are absolute, from ra8_lvd_regs.h (HUM Ch 8.2).

const ev = @import("lvd_events.zig");

pub const Map = ev.Map;

pub const codes = struct {
    pub const ok: u16 = 0;
    pub const invalid_arg: u16 = 0x103;
    pub const not_supported: u16 = 0x107;
    pub const null_ptr: u16 = 0x504;
};

pub const map = [4]Map{
    .{ .cmpcr = 0x4001_EA58, .cr0 = 0x4001_EA70, .cr1 = 0x4001_E0E0, .sr = 0x4001_E0E1, .fcr = 0x4001_EB20, .has_irq = true },
    .{ .cmpcr = 0x4001_EA5C, .cr0 = 0x4001_EA74, .cr1 = 0x4001_E0E2, .sr = 0x4001_E0E3, .fcr = 0x4001_EB24, .has_irq = true },
    .{ .cmpcr = 0x4001_EA64, .cr0 = 0x4001_EA7C, .cr1 = 0x4001_EA7C, .sr = 0x4001_EA7C, .fcr = 0x4001_EB2C, .has_irq = false },
    .{ .cmpcr = 0x4001_EA68, .cr0 = 0x4001_EA80, .cr1 = 0x4001_EA80, .sr = 0x4001_EA80, .fcr = 0x4001_EB30, .has_irq = false },
};

pub const cr0 = struct {
    pub const rie: u8 = 0x01;
    pub const dfdis: u8 = 0x02;
    pub const cmpe: u8 = 0x04;
    pub const bit3: u8 = 0x08;
    pub const fsamp: u8 = 0x30;
    pub const ri: u8 = 0x40;
    pub const n_bit6: u8 = 0x40;
    pub const rn: u8 = 0x80;
};

pub const pvdlvl_mask: u8 = 0x1F;
pub const pvde: u8 = 0x80;
pub const idtsel: u8 = 0x03;
pub const irqsel: u8 = 0x04;

pub const response = struct {
    pub const none: u8 = 0;
    pub const interrupt: u8 = 1;
    pub const nmi: u8 = 2;
    pub const reset: u8 = 3;
    pub const reset_on_rise: u8 = 4;
};

pub const irq_maskable: u8 = 1;
pub const negate_after_assert: u8 = 1;
pub const hysteresis_hvd: u8 = 1;

pub const Cfg = extern struct {
    threshold: u8,
    edge: u8,
    irq_type: u8,
    response: u8,
    negate: u8,
    hysteresis: u8,
    filter_div: u8,
    filter_en: bool,
    irq_enable: bool,
    clear_status: bool,
};

comptime {
    if (@sizeOf(Cfg) != 10 or @offsetOf(Cfg, "clear_status") != 9) @compileError("Cfg");
}

pub fn channelIdx(channel: u8) ?u8 {
    return switch (channel) {
        1 => 0,
        2 => 1,
        4 => 2,
        5 => 3,
        else => null,
    };
}

pub fn thresholdOk(t: u8) bool {
    return t >= 0x03 and t <= 0x0F;
}

pub fn divOk(d: u8) bool {
    return d <= 3;
}

pub fn edgeOk(e: u8) bool {
    return e <= 2;
}

pub fn rejectHvdAfter(hvd: u32, after_assert: u32, hysteresis: u32, negate: u32) bool {
    return hysteresis == hvd and negate == after_assert;
}

pub fn setRiBit(reset_val: u32, reset_on_rise_val: u32, resp: u32) bool {
    return resp == reset_val or resp == reset_on_rise_val;
}

pub fn validate(m: Map, c: Cfg) u16 {
    if (!thresholdOk(c.threshold) or !divOk(c.filter_div)) return codes.invalid_arg;
    if (m.has_irq) {
        if (!edgeOk(c.edge)) return codes.invalid_arg;
    } else if (c.response == response.interrupt or c.response == response.nmi) {
        return codes.not_supported;
    }
    if (rejectHvdAfter(hysteresis_hvd, negate_after_assert, c.hysteresis, c.negate)) return codes.invalid_arg;
    return codes.ok;
}

/// m channels write 1 to reserved bit3, n channels to reserved bit6.
pub fn withReserved(m: Map, v: u8) u8 {
    return v | (if (m.has_irq) cr0.bit3 else cr0.n_bit6);
}

pub fn composeCr0(m: Map, c: Cfg) u8 {
    var v: u8 = ((c.filter_div << 4) & cr0.fsamp) | cr0.dfdis;
    if (m.has_irq) {
        if (c.negate == negate_after_assert) v |= cr0.rn;
        if (setRiBit(response.reset, response.reset_on_rise, c.response)) v |= cr0.ri;
    }
    return v;
}

pub fn cr1Of(c: Cfg) u8 {
    return (c.edge & idtsel) | (if (c.irq_type == irq_maskable) irqsel else 0);
}

pub fn cr0Rmw(hw: anytype, m: Map, clear: u8, set: u8) void {
    const prev = hw.read8(m.cr0);
    hw.write8(m.cr0, withReserved(m, (prev & ~clear) | set));
}

pub fn programCmpcr(hw: anytype, m: Map, c: Cfg) void {
    hw.write8(m.cr0, withReserved(m, 0));
    hw.write8(m.cmpcr, 0);
    const lvl = c.threshold & pvdlvl_mask;
    hw.write8(m.cmpcr, lvl);
    hw.write8(m.fcr, c.hysteresis & 0x01);
    hw.write8(m.cmpcr, lvl | pvde);
}

pub fn programCr0Chain(hw: anytype, m: Map, c: Cfg) void {
    var v = composeCr0(m, c);
    hw.write8(m.cr0, withReserved(m, v));
    if (c.filter_en) {
        v &= ~cr0.dfdis;
        hw.write8(m.cr0, withReserved(m, v));
    }
    if (m.has_irq) {
        hw.write8(m.cr1, cr1Of(c));
        if (c.clear_status) hw.write8(m.sr, 0);
    }
    if (c.irq_enable and c.response != response.none) {
        v |= cr0.rie;
        hw.write8(m.cr0, withReserved(m, v));
    }
    v |= cr0.cmpe;
    hw.write8(m.cr0, withReserved(m, v));
}

/// HUM Table 8.5 / 8.7 stop order: CMPE, RIE/RE, DFDIS, PVDE, then the rest.
pub fn deinit(hw: anytype, m: Map) void {
    cr0Rmw(hw, m, cr0.cmpe, 0);
    cr0Rmw(hw, m, cr0.rie, 0);
    cr0Rmw(hw, m, 0, cr0.dfdis);
    hw.write8(m.cmpcr, 0);
    hw.write8(m.cr0, withReserved(m, 0));
    hw.write8(m.fcr, 0);
    if (m.has_irq) {
        hw.write8(m.cr1, 0);
        hw.write8(m.sr, 0);
    }
}

pub fn setThreshold(hw: anytype, m: Map, t: u8) void {
    const was = hw.read8(m.cmpcr) & pvde;
    hw.write8(m.cmpcr, 0);
    hw.write8(m.cmpcr, (t & pvdlvl_mask) | was);
}

pub fn setEdge(hw: anytype, m: Map, e: u8) void {
    hw.write8(m.cr1, (hw.read8(m.cr1) & ~idtsel) | (e & idtsel));
}

pub fn setKind(hw: anytype, m: Map, kind: u8) void {
    const v = hw.read8(m.cr1) & ~irqsel;
    hw.write8(m.cr1, v | (if (kind == irq_maskable) irqsel else 0));
}
