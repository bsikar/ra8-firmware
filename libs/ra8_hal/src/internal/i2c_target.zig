//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! RIIC target (peripheral) role, ported from ra8_i2c_peripheral.c
//! (RA8FW-769). Every op takes a *volatile Regs, so host tests can drive
//! a plain struct. HUM Ch 39.2 (registers) and Ch 39.3.5 (target flow).

/// Mirror of r_i2c_regs_t (22 bytes, one byte per register).
pub const Regs = extern struct {
    iccr1: u8 = 0,
    iccr2: u8 = 0,
    icmr1: u8 = 0,
    icmr2: u8 = 0,
    icmr3: u8 = 0,
    icfer: u8 = 0,
    icser: u8 = 0,
    icier: u8 = 0,
    icsr1: u8 = 0,
    icsr2: u8 = 0,
    sarl0: u8 = 0,
    saru0: u8 = 0,
    sarl1: u8 = 0,
    saru1: u8 = 0,
    sarl2: u8 = 0,
    saru2: u8 = 0,
    icbrl: u8 = 0,
    icbrh: u8 = 0,
    icdrt: u8 = 0,
    icdrr: u8 = 0,
    icwur: u8 = 0,
    icwur2: u8 = 0,
};

comptime {
    if (@sizeOf(Regs) != 22) @compileError("r_i2c_regs_t is 22 bytes");
    if (@offsetOf(Regs, "icser") != 0x06) @compileError("ICSER at +0x06");
    if (@offsetOf(Regs, "icsr2") != 0x09) @compileError("ICSR2 at +0x09");
    if (@offsetOf(Regs, "sarl0") != 0x0A) @compileError("SARL0 at +0x0A");
    if (@offsetOf(Regs, "icdrt") != 0x12) @compileError("ICDRT at +0x12");
    if (@offsetOf(Regs, "icdrr") != 0x13) @compileError("ICDRR at +0x13");
}

pub const msk = struct {
    pub const icsr1_match: u8 = 0x0F; // AAS0..2 | GCA
    pub const icsr2_stop: u8 = 0x08;
    pub const icsr2_nackf: u8 = 0x10;
    pub const icsr2_rdrf: u8 = 0x20;
    pub const icsr2_tend: u8 = 0x40;
    pub const icsr2_tdre: u8 = 0x80;
    pub const iccr2_trs: u8 = 0x20;
    pub const icmr3_wait: u8 = 0x40;
    pub const icser_sar0e: u8 = 0x01;
    pub const icser_gcae: u8 = 0x08;
    pub const icier_arm: u8 = 0xA8; // RIE | TIE | SPIE
};

pub const Event = struct {
    pub const none: u8 = 0;
    pub const write: u8 = 1;
    pub const read: u8 = 2;
    pub const stop: u8 = 3;
};

pub const poll_limit: u32 = 200000;
pub const addr_7b_max: u8 = 0x7F;
pub const slot_max: u8 = 2;

pub fn pollDone(icsr1: u8, icsr2: u8) bool {
    return (icsr1 & msk.icsr1_match) != 0 or (icsr2 & msk.icsr2_stop) != 0;
}

pub fn rxContinue(icsr2: u8, received: u32, capacity: u32) bool {
    return (icsr2 & msk.icsr2_stop) == 0 and received < capacity;
}

pub fn txDone(icsr2: u8) bool {
    return (icsr2 & msk.icsr2_nackf) != 0 or (icsr2 & msk.icsr2_tend) != 0;
}

pub fn txContinue(icsr2: u8, sent: u32, len: u32) bool {
    return (icsr2 & msk.icsr2_nackf) == 0 and sent < len;
}

pub fn classify(icsr1: u8, iccr2: u8) u8 {
    if ((icsr1 & msk.icsr1_match) == 0) return Event.none;
    if ((iccr2 & msk.iccr2_trs) != 0) return Event.read;
    return Event.write;
}

pub fn dispatchEvent(icsr1: u8, icsr2: u8, iccr2: u8) u8 {
    const match = classify(icsr1, iccr2);
    if (match != Event.none) return match;
    if ((icsr2 & msk.icsr2_stop) != 0) return Event.stop;
    return Event.none;
}

pub fn icserMask(slot: u8, general_call: bool) u8 {
    var mask: u8 = msk.icser_sar0e << @intCast(slot);
    if (general_call) mask |= msk.icser_gcae;
    return mask;
}

/// 7-bit own address into SARLy (SVA at bit 1), SARUy.FS = 0.
pub fn setAddr(r: *volatile Regs, slot: u8, addr_7b: u8) void {
    const sarl: u8 = addr_7b << 1;
    switch (slot) {
        0 => {
            r.sarl0 = sarl;
            r.saru0 = 0;
        },
        1 => {
            r.sarl1 = sarl;
            r.saru1 = 0;
        },
        else => {
            r.sarl2 = sarl;
            r.saru2 = 0;
        },
    }
}

pub const Setup = struct {
    slot: u8,
    addr_7b: u8,
    general_call: bool,
    clock_stretch: bool,
    irq_enable: bool,
};

pub fn arm(r: *volatile Regs, s: Setup) void {
    setAddr(r, s.slot, s.addr_7b);
    r.icser = icserMask(s.slot, s.general_call);
    if (s.clock_stretch) r.icmr3 = r.icmr3 | msk.icmr3_wait;
    if (s.irq_enable) r.icier = r.icier | msk.icier_arm;
}

pub fn disarm(r: *volatile Regs) void {
    r.icser = 0;
    r.icmr3 = r.icmr3 & ~msk.icmr3_wait;
    r.icier = r.icier & ~msk.icier_arm;
}

/// Spin up to poll_limit reads of ICSR2; true once any bit in `mask` is set.
pub fn wait(r: *const volatile Regs, mask: u8) bool {
    var i: u32 = 0;
    while (i < poll_limit) : (i += 1) {
        if ((r.icsr2 & mask) != 0) return true;
    }
    return false;
}

/// Spin until an own-address match or STOP, then classify.
pub fn poll(r: *const volatile Regs) u8 {
    var i: u32 = 0;
    while (i < poll_limit) : (i += 1) {
        const icsr1 = r.icsr1;
        const icsr2 = r.icsr2;
        if (pollDone(icsr1, icsr2)) break;
    }
    const icsr1 = r.icsr1;
    const iccr2 = r.iccr2;
    return classify(icsr1, iccr2);
}

pub const Rx = struct { count: u32, timed_out: bool };

/// Drain bytes until STOP or `buf` is full; one extra pass sees the STOP.
pub fn drainRx(r: *volatile Regs, buf: []u8) Rx {
    const capacity: u32 = @intCast(buf.len);
    var count: u32 = 0;
    var i: u64 = 0;
    while (i <= capacity) : (i += 1) {
        if (!wait(r, msk.icsr2_rdrf | msk.icsr2_stop)) return .{ .count = count, .timed_out = true };
        const icsr2 = r.icsr2;
        const full = (icsr2 & msk.icsr2_rdrf) != 0;
        if (!rxContinue(icsr2, count, capacity)) {
            if (full and count < capacity) {
                buf[count] = r.icdrr;
                count += 1;
            }
            break;
        }
        if (full) {
            buf[count] = r.icdrr;
            count += 1;
        }
    }
    return .{ .count = count, .timed_out = false };
}

/// Load ICDRT on each TDRE until NACK or all of `data` is sent.
pub fn fillTx(r: *volatile Regs, data: []const u8) u32 {
    const len: u32 = @intCast(data.len);
    var sent: u32 = 0;
    var i: u64 = 0;
    while (i <= len) : (i += 1) {
        if (!wait(r, msk.icsr2_tdre)) break;
        if (!txContinue(r.icsr2, sent, len)) break;
        r.icdrt = data[sent];
        sent += 1;
    }
    return sent;
}

/// Wait for TEND or NACKF, dummy-read ICDRR to release SCL, clear NACKF and STOP.
pub fn finishTx(r: *volatile Regs) bool {
    _ = wait(r, msk.icsr2_tend | msk.icsr2_nackf);
    const ended = txDone(r.icsr2);
    _ = r.icdrr;
    r.icsr2 = r.icsr2 & ~(msk.icsr2_nackf | msk.icsr2_stop);
    return ended;
}
