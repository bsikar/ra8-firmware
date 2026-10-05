//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/cnecc.zig: CTL encoders, status decode, CRC32,
//! the register write order and the dispatch counters.

const std = @import("std");
const cn = @import("cnecc");

const Write = struct { addr: usize, val: u32 };

const Rec = struct {
    log: [16]Write = undefined,
    n: usize = 0,
    pub fn write16(self: *Rec, a: usize, v: u16) void {
        self.log[self.n] = .{ .addr = a, .val = v };
        self.n += 1;
    }
    pub fn write32(self: *Rec, a: usize, v: u32) void {
        self.log[self.n] = .{ .addr = a, .val = v };
        self.n += 1;
    }
    fn expect(self: *Rec, want: []const Write) !void {
        try std.testing.expectEqualSlices(Write, want, self.log[0..self.n]);
    }
};

test "ctlValue sets IRQ enables, inverted correction, EMCA unlock and ECERVF" {
    const all = cn.InstanceCfg{ .correct_1bit = true, .irq_1bit = true, .irq_2bit = true, .enable = true };
    try std.testing.expectEqual(@as(u32, 0x4058), cn.ctlValue(all));
    const none = cn.InstanceCfg{ .correct_1bit = false, .irq_1bit = false, .irq_2bit = false, .enable = false };
    try std.testing.expectEqual(@as(u32, 0x4020), cn.ctlValue(none));
    try std.testing.expectEqual(@as(u32, 0x18), cn.irqBits(true, true));
    try std.testing.expectEqual(@as(u32, 0x10), cn.irqBits(false, true));
}

test "ctlRmw drops status and W1C bits and re-asserts EMCA 01b" {
    // Live: every status flag, both W1C bits, EMCA 11b, ECERVF and EC1EDIC.
    const live: u32 = 0x0003_CE4F;
    // Writable keeps bits 6..0 (as the C mask 0xC67F does); ECOVFF, the
    // address flags and the W1C bits never echo back.
    try std.testing.expectEqual(@as(u32, 0x400F), cn.ctlRmw(live, 0, cn.ctl.ecervf));
    try std.testing.expectEqual(@as(u32, 0x4057), cn.ctlRmw(live, cn.ctl.ec2edic, cn.ctl.irq_all));
    try std.testing.expectEqual(@as(u32, 0x4060), cn.ctlRmw(0x4040, cn.ctl.ec1ecp, cn.ctl.ec1ecp));
}

test "decode maps CTL, TMC and EAD onto the status mirror" {
    const s = cn.decode(0x0003_0877, 0x8082, 0xFFFF_F6AB, .{ .one_bit_count = 3, .two_bit_count = 2, .overflow_count = 1 });
    try std.testing.expectEqual(@as(u16, 0x2AB), s.last_addr);
    try std.testing.expect(s.err_present and s.err_1bit and s.err_2bit and s.overflow);
    try std.testing.expect(s.addr_is_1bit and s.addr_is_2bit and s.judgment_active);
    try std.testing.expect(!s.correct_enabled and !s.irq1_enabled and s.irq2_enabled and s.test_mode);
    try std.testing.expectEqual(@as(u32, 2), s.two_bit_count);
    const q = cn.decode(0x4008, 0x8000, 0, .{});
    try std.testing.expect(q.correct_enabled and q.irq1_enabled and !q.test_mode and !q.reserved1);
}

test "crc32 matches the standard check value" {
    try std.testing.expectEqual(@as(u32, 0xCBF4_3926), cn.crc32("123456789"));
    try std.testing.expectEqual(@as(u32, 0), cn.crc32(""));
    try std.testing.expectEqual(@as(u32, 8), cn.alignedLen(11));
    try std.testing.expectEqual(cn.codes.null_ptr, cn.computeCheck(0, 8));
    try std.testing.expectEqual(cn.codes.invalid_arg, cn.computeCheck(0x2000_0002, 8));
    try std.testing.expectEqual(cn.codes.invalid_arg, cn.computeCheck(0x2000_0000, 3));
    try std.testing.expectEqual(cn.codes.ok, cn.computeCheck(0x2000_0000, 4));
}

test "apply, standby and inject write in the HUM order" {
    var r = Rec{};
    const b1 = cn.base(1);
    try std.testing.expectEqual(@as(usize, 0x4036_F300), b1);
    cn.applyRegs(&r, 1, .{ .correct_1bit = true, .irq_1bit = false, .irq_2bit = true, .enable = true });
    cn.standbyRegs(&r, 1);
    cn.injectRegs(&r, 1, 0xDEAD_BEEF);
    try r.expect(&.{
        .{ .addr = b1, .val = 0x600 },
        .{ .addr = b1, .val = 0x4050 },
        .{ .addr = b1 + 4, .val = 0x8000 },
        .{ .addr = b1, .val = 0x600 },
        .{ .addr = b1, .val = 0x4000 },
        .{ .addr = b1 + 4, .val = 0x8000 },
        .{ .addr = b1 + 0xC, .val = 0xDEAD_BEEF },
        .{ .addr = b1 + 4, .val = 0x8080 },
        .{ .addr = b1 + 4, .val = 0x8082 },
    });
}

var seen: struct { ctx: ?*anyopaque = null, inst: u8 = 0xFF, two: bool = false, addr: u16 = 0, calls: u32 = 0 } = .{};

fn onError(ctx: ?*anyopaque, inst: u8, two: bool, addr: u16) callconv(.C) void {
    seen = .{ .ctx = ctx, .inst = inst, .two = two, .addr = addr, .calls = seen.calls + 1 };
}

test "dispatch counts each kind, mirrors it and calls the handler" {
    var st = cn.State{};
    var m = cn.Counters{ .one_bit_count = 99 };
    st.setMirror(1, &m);
    try std.testing.expectEqual(@as(u32, 0), m.one_bit_count);
    var tagv: u8 = 0;
    st.handler = &onError;
    st.ctx = &tagv;
    st.dispatch(1, true, 0x123);
    st.dispatch(1, false, 0x3FF);
    st.dispatchOverflow(1);
    st.dispatch(2, true, 0);
    st.dispatchOverflow(2);
    try std.testing.expectEqual(cn.Counters{ .one_bit_count = 1, .two_bit_count = 1, .overflow_count = 1 }, st.counts[1]);
    try std.testing.expectEqual(st.counts[1], m);
    try std.testing.expectEqual(@as(u32, 2), seen.calls);
    try std.testing.expect(seen.inst == 1 and !seen.two and seen.addr == 0x3FF and seen.ctx == @as(?*anyopaque, &tagv));
}

test "reset clears counters and mirror; clearCounts leaves the mirror" {
    var st = cn.State{};
    var m = cn.Counters{};
    st.setMirror(0, &m);
    st.dispatchOverflow(0);
    st.clearCounts(0);
    try std.testing.expectEqual(cn.Counters{}, st.counts[0]);
    try std.testing.expectEqual(@as(u32, 1), m.overflow_count);
    st.counts[0].two_bit_count = std.math.maxInt(u32);
    st.dispatch(0, true, 0);
    try std.testing.expectEqual(@as(u32, 0), st.counts[0].two_bit_count);
    st.resetCounts(0);
    try std.testing.expectEqual(cn.Counters{}, m);
    try std.testing.expectEqual(@as(u16, (2 << 8) | 27), cn.mstp_ids[0]);
    try std.testing.expectEqual(@as(u16, 0x339), cn.events[1]);
}
