//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! RIIC target role (RA8FW-769).

const std = @import("std");
const t = @import("i2c_target");

const expect = std.testing.expect;
const eq = std.testing.expectEqual;

test "predicates match the C decisions" {
    try expect(!t.pollDone(0, 0));
    try expect(t.pollDone(0x01, 0));
    try expect(t.pollDone(0x08, 0));
    try expect(t.pollDone(0, t.msk.icsr2_stop));
    try expect(t.rxContinue(0, 0, 4));
    try expect(!t.rxContinue(t.msk.icsr2_stop, 0, 4));
    try expect(!t.rxContinue(0, 4, 4));
    try expect(t.txDone(t.msk.icsr2_tend));
    try expect(t.txDone(t.msk.icsr2_nackf));
    try expect(!t.txDone(t.msk.icsr2_tdre));
    try expect(t.txContinue(0, 1, 2));
    try expect(!t.txContinue(t.msk.icsr2_nackf, 0, 2));
    try expect(!t.txContinue(0, 2, 2));
}

test "classify and dispatch events" {
    try eq(t.Event.none, t.classify(0, t.msk.iccr2_trs));
    try eq(t.Event.write, t.classify(0x02, 0));
    try eq(t.Event.read, t.classify(0x04, t.msk.iccr2_trs));
    try eq(t.Event.stop, t.dispatchEvent(0, t.msk.icsr2_stop, 0));
    try eq(t.Event.write, t.dispatchEvent(0x01, t.msk.icsr2_stop, 0));
    try eq(t.Event.none, t.dispatchEvent(0, 0, 0));
}

test "own address and ICSER per slot" {
    try eq(@as(u8, 0x01), t.icserMask(0, false));
    try eq(@as(u8, 0x0C), t.icserMask(2, true));
    var r = t.Regs{ .saru1 = 0xFF };
    t.setAddr(&r, 1, 0x50);
    try eq(@as(u8, 0xA0), r.sarl1);
    try eq(@as(u8, 0), r.saru1);
    try eq(@as(u8, 0), r.sarl0);
    t.setAddr(&r, 2, 0x7F);
    try eq(@as(u8, 0xFE), r.sarl2);
}

test "arm and disarm touch only their bits" {
    var r = t.Regs{ .icmr3 = 0x01, .icier = 0x02 };
    t.arm(&r, .{ .slot = 0, .addr_7b = 0x12, .general_call = true, .clock_stretch = true, .irq_enable = true });
    try eq(@as(u8, 0x24), r.sarl0);
    try eq(@as(u8, 0x09), r.icser);
    try eq(@as(u8, 0x41), r.icmr3);
    try eq(@as(u8, 0xAA), r.icier);
    t.disarm(&r);
    try eq(@as(u8, 0), r.icser);
    try eq(@as(u8, 0x01), r.icmr3);
    try eq(@as(u8, 0x02), r.icier);
}

test "wait times out with no flag and poll classifies a match" {
    var r = t.Regs{};
    try expect(!t.wait(&r, t.msk.icsr2_rdrf));
    r.icsr2 = t.msk.icsr2_rdrf;
    try expect(t.wait(&r, t.msk.icsr2_rdrf));
    r.icsr1 = 0x01;
    r.iccr2 = t.msk.iccr2_trs;
    try eq(t.Event.read, t.poll(&r));
}

test "drainRx fills the buffer while RDRF stays set" {
    var r = t.Regs{ .icsr2 = t.msk.icsr2_rdrf, .icdrr = 0x5A };
    var buf: [3]u8 = @splat(0);
    const rx = t.drainRx(&r, &buf);
    try eq(@as(u32, 3), rx.count);
    try expect(!rx.timed_out);
    try eq([_]u8{ 0x5A, 0x5A, 0x5A }, buf);
}

test "drainRx stops on STOP and keeps a last byte" {
    var r = t.Regs{ .icsr2 = t.msk.icsr2_stop, .icdrr = 0x11 };
    var buf: [2]u8 = @splat(0);
    try eq(@as(u32, 0), t.drainRx(&r, &buf).count);
    r.icsr2 = t.msk.icsr2_stop | t.msk.icsr2_rdrf;
    const rx = t.drainRx(&r, &buf);
    try eq(@as(u32, 1), rx.count);
    try eq(@as(u8, 0x11), buf[0]);
}

test "fillTx sends all bytes and finishTx clears NACKF and STOP" {
    var r = t.Regs{ .icsr2 = t.msk.icsr2_tdre };
    try eq(@as(u32, 3), t.fillTx(&r, &[_]u8{ 1, 2, 0x33 }));
    try eq(@as(u8, 0x33), r.icdrt);
    r.icsr2 = t.msk.icsr2_tend | t.msk.icsr2_nackf | t.msk.icsr2_stop;
    try expect(t.finishTx(&r));
    try eq(t.msk.icsr2_tend, r.icsr2);
}

test "fillTx stops on NACK" {
    var r = t.Regs{ .icsr2 = t.msk.icsr2_tdre | t.msk.icsr2_nackf };
    try eq(@as(u32, 0), t.fillTx(&r, &[_]u8{ 1, 2 }));
    r.icsr2 = 0;
    try expect(!t.finishTx(&r));
}
