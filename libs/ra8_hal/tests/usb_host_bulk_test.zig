//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const hb = @import("usb_host_bulk");

const Fake = struct {
    regs: [0x82 / 2]u16 align(4) = @splat(0),
    present: bool = true,
    devadd: ?u8 = null,
    quiesced: u8 = 0,
    rmw_set: u16 = 0,
    pids: [4]u16 = .{ 0xFF, 0xFF, 0xFF, 0xFF },
    npid: u8 = 0,
    frdy_err: u16 = hb.ok,
    queue_err: u16 = hb.ok,
    queued: u16 = 0,
    packets: []const u16 = &.{},
    next: usize = 0,
    nulls: u8 = 0,

    fn base(f: *Fake) usize {
        return @intFromPtr(&f.regs);
    }
    fn reg(f: *Fake, off: usize) u16 {
        return f.regs[off / 2];
    }
    fn set(f: *Fake, off: usize, v: u16) void {
        f.regs[off / 2] = v;
    }
    pub fn pick(f: *Fake, speed: u8) ?usize {
        if (!f.present or speed > 1) return null;
        return f.base();
    }
    pub fn programDevadd(f: *Fake, _: usize, dev_addr: u8) void {
        f.devadd = dev_addr;
    }
    pub fn pipeQuiesce(f: *Fake, _: usize, _: u8) void {
        f.quiesced += 1;
    }
    pub fn pipecfgWord(_: *Fake, ep: u8, dir: u8, ep_type: u8, dblb: bool) u16 {
        return @as(u16, ep) | (@as(u16, dir) << 4) | (@as(u16, ep_type) << 8) | (@as(u16, @intFromBool(dblb)) << 12);
    }
    pub fn pipebufWord(_: *Fake, pipe: u8, max_packet: u16) u16 {
        return @as(u16, pipe) << 10 | (max_packet / 64);
    }
    pub fn rmw16(f: *Fake, r: *volatile u16, s: u16, c: u16) void {
        f.rmw_set = s;
        r.* = (r.* & ~c) | s;
    }
    pub fn pipePid(f: *Fake, _: usize, _: u8, pid: u16) void {
        f.pids[f.npid] = pid;
        f.npid += 1;
    }
    pub fn selectCfifo(_: *Fake, _: usize, _: u8, _: bool) void {}
    pub fn waitFrdy(f: *Fake, _: usize) u16 {
        if (f.frdy_err != hb.ok) return f.frdy_err;
        f.set(hb.off.cfifoctr, f.packets[f.next]);
        f.next += 1;
        return hb.ok;
    }
    pub fn fifoRead(f: *Fake, _: usize, dst: [*]u8, len: u16) void {
        for (dst[0..len], 0..) |*b, i| b.* = @truncate(i + 0x10 * f.next);
        // The next packet is ready as soon as this one drains.
        f.set(hb.off.brdysts, f.reg(hb.off.brdysts) | 0x0004);
    }
    pub fn queueIn(f: *Fake, _: u8, pipe: u8, _: [*]const u8, len: u16) u16 {
        f.queued = len;
        if (f.queue_err == hb.ok) f.set(hb.off.bempsts, f.reg(hb.off.bempsts) | (@as(u16, 1) << @as(u4, @truncate(pipe))));
        return f.queue_err;
    }
    pub fn nullPtr(f: *Fake, _: [*:0]const u8) u16 {
        f.nulls += 1;
        return hb.null_ptr;
    }
};

test "setTarget programs DEVADD and DCPMAXP.DEVSEL keeping MXPS" {
    var f = Fake{};
    f.set(hb.off.dcpmaxp, 0xF040);
    try std.testing.expectEqual(hb.ok, hb.setTarget(&f, 0, 3));
    try std.testing.expectEqual(@as(?u8, 3), f.devadd);
    try std.testing.expectEqual(@as(u16, 0x3040), f.reg(hb.off.dcpmaxp));
    try std.testing.expectEqual(hb.invalid_arg, hb.setTarget(&f, 0, 11));
    try std.testing.expectEqual(hb.invalid_arg, hb.setTarget(&f, 2, 1));
}

test "pipeArgsOk bounds every field" {
    try std.testing.expectEqual(hb.ok, hb.pipeArgsOk(1, 10, 15, 0x7FF));
    try std.testing.expectEqual(hb.invalid_arg, hb.pipeArgsOk(0, 1, 1, 64));
    try std.testing.expectEqual(hb.invalid_arg, hb.pipeArgsOk(10, 1, 1, 64));
    try std.testing.expectEqual(hb.invalid_arg, hb.pipeArgsOk(1, 11, 1, 64));
    try std.testing.expectEqual(hb.invalid_arg, hb.pipeArgsOk(1, 1, 0, 64));
    try std.testing.expectEqual(hb.invalid_arg, hb.pipeArgsOk(1, 1, 16, 64));
    try std.testing.expectEqual(hb.invalid_arg, hb.pipeArgsOk(1, 1, 1, 0));
    try std.testing.expectEqual(hb.invalid_arg, hb.pipeArgsOk(1, 1, 1, 0x800));
}

test "pipeSetup programs the pipe and clears its status bits" {
    var f = Fake{};
    f.set(hb.off.pipectr + 2, 0);
    try std.testing.expectEqual(hb.ok, hb.pipeSetup(&f, 1, 2, 5, 1, true, 512));
    try std.testing.expectEqual(@as(u8, 1), f.quiesced);
    try std.testing.expectEqual(@as(u16, 0x1001), f.reg(hb.off.pipecfg));
    try std.testing.expectEqual(@as(u16, (2 << 10) | 8), f.reg(hb.off.pipebuf));
    try std.testing.expectEqual(@as(u16, 0x5200), f.reg(hb.off.pipemaxp));
    try std.testing.expectEqual(@as(u16, 0), f.reg(hb.off.pipesel));
    try std.testing.expectEqual(hb.dcpctr_sqclr, f.reg(hb.off.pipectr + 2));
    try std.testing.expectEqual(@as(u16, 0xFFFB), f.reg(hb.off.brdysts));
    try std.testing.expectEqual(@as(u16, 0xFFFB), f.reg(hb.off.nrdysts));
    try std.testing.expectEqual(@as(u16, 0xFFFB), f.reg(hb.off.bempsts));
    try std.testing.expectEqual(hb.ok, hb.pipeSetup(&f, 1, 2, 5, 1, false, 512));
    try std.testing.expectEqual(@as(u16, 0x1011), f.reg(hb.off.pipecfg));
}

test "pipeSetup rejects a missing controller and bad args before touching it" {
    var f = Fake{ .present = false };
    try std.testing.expectEqual(hb.invalid_arg, hb.pipeSetup(&f, 0, 1, 1, 1, true, 64));
    f.present = true;
    try std.testing.expectEqual(hb.invalid_arg, hb.pipeSetup(&f, 0, 1, 1, 1, true, 0));
    try std.testing.expectEqual(@as(u8, 0), f.quiesced);
}

test "bulkOut queues the data, waits for BEMP and parks the pipe at NAK" {
    var f = Fake{};
    const data = [_]u8{ 1, 2, 3 };
    try std.testing.expectEqual(hb.ok, hb.bulkOut(&f, 0, 1, &data, 3));
    try std.testing.expectEqual(@as(u16, 3), f.queued);
    try std.testing.expectEqual(@as(u16, 0x0002), f.reg(hb.off.bempenb));
    try std.testing.expectEqual(@as(u16, 0xFFFD), f.reg(hb.off.bempsts));
    try std.testing.expectEqual(hb.pid_nak, f.pids[0]);
}

test "bulkOut reports nulls, range and queue errors" {
    var f = Fake{};
    const data = [_]u8{1};
    try std.testing.expectEqual(hb.null_ptr, hb.bulkOut(&f, 0, 1, null, 1));
    try std.testing.expectEqual(hb.invalid_arg, hb.bulkOut(&f, 2, 1, &data, 1));
    try std.testing.expectEqual(hb.invalid_arg, hb.bulkOut(&f, 0, 0, &data, 1));
    try std.testing.expectEqual(hb.invalid_arg, hb.bulkOut(&f, 0, 10, &data, 1));
    f.queue_err = hb.invalid_state;
    try std.testing.expectEqual(hb.invalid_state, hb.bulkOut(&f, 0, 1, &data, 1));
    try std.testing.expectEqual(@as(u8, 0), f.npid);
}

test "bulkOut returns hw_error when the pipe STALLs" {
    var f = Fake{ .queue_err = hb.ok };
    f.set(hb.off.pipectr + 2, hb.pid_stall_bit);
    const data = [_]u8{1};
    // queueIn raises BEMP for pipe 2, so stall a pipe whose BEMP stays low.
    f.set(hb.off.pipectr + 4, hb.pid_stall_bit);
    f.queue_err = hb.ok;
    var g = Fake{};
    g.set(hb.off.pipectr, hb.pid_stall_bit);
    g.queue_err = hb.ok;
    g.regs[hb.off.bempsts / 2] = 0;
    const Silent = struct {
        inner: *Fake,
        pub fn pick(s: @This(), sp: u8) ?usize {
            return s.inner.pick(sp);
        }
        pub fn queueIn(_: @This(), _: u8, _: u8, _: [*]const u8, _: u16) u16 {
            return hb.ok;
        }
        pub fn pipePid(s: @This(), b: usize, p: u8, pid: u16) void {
            s.inner.pipePid(b, p, pid);
        }
        pub fn nullPtr(s: @This(), m: [*:0]const u8) u16 {
            return s.inner.nullPtr(m);
        }
    };
    try std.testing.expectEqual(hb.hw_error, hb.bulkOut(Silent{ .inner = &g }, 0, 1, &data, 1));
    try std.testing.expectEqual(hb.pid_nak, g.pids[0]);
}

test "bulkIn drains packets until a short one and parks at NAK" {
    var f = Fake{ .packets = &.{ 64, 10 } };
    f.set(hb.off.pipemaxp, 64);
    f.set(hb.off.brdysts, 0x0004);
    var buf: [128]u8 = undefined;
    var got: u16 = 0;
    try std.testing.expectEqual(hb.ok, hb.bulkIn(&f, 0, 2, &buf, 128, &got));
    try std.testing.expectEqual(@as(u16, 74), got);
    try std.testing.expectEqual(@as(u16, 0x0004), f.reg(hb.off.brdyenb));
    try std.testing.expectEqual(hb.pid_buf, f.pids[0]);
    try std.testing.expectEqual(hb.pid_nak, f.pids[1]);
    try std.testing.expectEqual(@as(u8, 0x10), buf[0]);
    try std.testing.expectEqual(@as(u8, 0x20), buf[64]);
}

test "bulkIn clears the FIFO on an overrun and on a zero-length packet" {
    var f = Fake{ .packets = &.{40} };
    f.set(hb.off.pipemaxp, 64);
    f.set(hb.off.brdysts, 0x0004);
    var buf: [16]u8 = undefined;
    var got: u16 = 0;
    try std.testing.expectEqual(hb.ok, hb.bulkIn(&f, 0, 2, &buf, 16, &got));
    try std.testing.expectEqual(@as(u16, 16), got);
    try std.testing.expectEqual(hb.fifoctr_bclr, f.reg(hb.off.cfifoctr));
    var z = Fake{ .packets = &.{0} };
    z.set(hb.off.pipemaxp, 64);
    z.set(hb.off.brdysts, 0x0004);
    try std.testing.expectEqual(hb.ok, hb.bulkIn(&z, 0, 2, &buf, 16, &got));
    try std.testing.expectEqual(@as(u16, 0), got);
    try std.testing.expectEqual(hb.fifoctr_bclr, z.reg(hb.off.cfifoctr));
}

test "bulkIn reports nulls, range, unset MPS and FRDY failures" {
    var f = Fake{};
    var buf: [8]u8 = undefined;
    var got: u16 = 7;
    try std.testing.expectEqual(hb.null_ptr, hb.bulkIn(&f, 0, 1, null, 8, &got));
    try std.testing.expectEqual(hb.null_ptr, hb.bulkIn(&f, 0, 1, &buf, 8, null));
    try std.testing.expectEqual(hb.invalid_arg, hb.bulkIn(&f, 3, 1, &buf, 8, &got));
    try std.testing.expectEqual(hb.invalid_arg, hb.bulkIn(&f, 0, 0, &buf, 8, &got));
    try std.testing.expectEqual(hb.invalid_state, hb.bulkIn(&f, 0, 1, &buf, 8, &got));
    f.set(hb.off.pipemaxp, 8);
    f.set(hb.off.brdysts, 0x0002);
    f.frdy_err = hb.hw_timeout;
    try std.testing.expectEqual(hb.hw_timeout, hb.bulkIn(&f, 0, 1, &buf, 8, &got));
    try std.testing.expectEqual(@as(u16, 0), got);
    try std.testing.expectEqual(hb.pid_nak, f.pids[1]);
}

test "lineState masks SYSSTS0.LNST and is zero without a controller" {
    var f = Fake{};
    f.set(hb.off.syssts0, 0xFFFE);
    try std.testing.expectEqual(@as(u16, 2), hb.lineState(&f, 0));
    try std.testing.expectEqual(@as(u16, 0), hb.lineState(&f, 5));
}
