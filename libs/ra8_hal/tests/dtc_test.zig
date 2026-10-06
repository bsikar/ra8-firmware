//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/dtc.zig.

const std = @import("std");
const dtc = @import("dtc");

/// DTC0 register block as bytes; records the order of 8-bit writes.
const Regs = struct {
    mem: [0x18]u8 = @splat(0),
    w8: [8][2]u8 = undefined,
    n8: usize = 0,

    pub fn read16(self: *Regs, off: usize) u16 {
        return std.mem.readInt(u16, self.mem[off..][0..2], .little);
    }
    pub fn write8(self: *Regs, off: usize, v: u8) void {
        self.w8[self.n8] = .{ @intCast(off), v };
        self.n8 += 1;
        self.mem[off] = v;
    }
    pub fn write16(self: *Regs, off: usize, v: u16) void {
        std.mem.writeInt(u16, self.mem[off..][0..2], v, .little);
    }
    pub fn write32(self: *Regs, off: usize, v: u32) void {
        std.mem.writeInt(u32, self.mem[off..][0..4], v, .little);
    }
    fn r32(self: *Regs, off: usize) u32 {
        return std.mem.readInt(u32, self.mem[off..][0..4], .little);
    }
};

const Ops = struct {
    mstp_err: u16 = 0,
    enabled: usize = 0,
    disabled: usize = 0,
    cleans: [4]usize = undefined,
    sizes: [4]u32 = undefined,
    nc: usize = 0,
    dtce: ?u16 = null,
    err: ?[]const u8 = null,
    info: usize = 0,
    fail_err: u16 = 0,

    pub fn mstpEnable(self: *Ops, id: u16) u16 {
        std.debug.assert(id == dtc.mstp_dmac0_dtc0);
        self.enabled += 1;
        return self.mstp_err;
    }
    pub fn mstpDisable(self: *Ops, _: u16) u16 {
        self.disabled += 1;
        return 0x77;
    }
    pub fn cacheClean(self: *Ops, addr: anytype, size: u32) u16 {
        self.cleans[self.nc] = @intFromPtr(addr);
        self.sizes[self.nc] = size;
        self.nc += 1;
        return 0;
    }
    pub fn isrSetDtc(self: *Ops, slot: u16, enable: bool) u16 {
        std.debug.assert(enable);
        self.dtce = slot;
        return 0;
    }
    pub fn nullPtr(self: *Ops, msg: [*:0]const u8) u16 {
        self.err = std.mem.span(msg);
        return dtc.null_ptr;
    }
    pub fn logError(self: *Ops, msg: [*:0]const u8) void {
        self.err = std.mem.span(msg);
    }
    pub fn logInfo(self: *Ops, _: [*:0]const u8) void {
        self.info += 1;
    }
    pub fn fail(self: *Ops, msg: [*:0]const u8, e: u16) void {
        self.err = std.mem.span(msg);
        self.fail_err = e;
    }
};

const Table = struct { entry: [dtc.vector_entries]u32 align(1024) = @splat(0) };

var src_buf: [8]u8 = undefined;
var dst_buf: [8]u8 = undefined;

fn cfgOf(mode: u8, unit: u8, units: u16, blocks: u16) dtc.Cfg {
    return .{ .src = &src_buf, .dst = &dst_buf, .src_mode = dtc.addr_inc, .dst_mode = dtc.addr_fixed, .unit = unit, .mode = mode, .unit_count = units, .block_count = blocks };
}

test "the TI block is 16 bytes with CRB then CRA" {
    try std.testing.expectEqual(@as(usize, 16), @sizeOf(dtc.Ti));
    try std.testing.expectEqual(@as(usize, 0x0C), @offsetOf(dtc.Ti, "crb"));
    try std.testing.expectEqual(@as(usize, 0x0E), @offsetOf(dtc.Ti, "cra"));
    try std.testing.expectEqual(@as(usize, 16), @offsetOf(dtc.Cfg, "src_mode"));
}

test "init programs both vector bases and stops the engine" {
    var r = Regs{};
    var o = Ops{};
    var t = Table{};
    var s = dtc.State{};
    try std.testing.expectEqual(dtc.ok, s.init(&r, &o, &t));
    const want: u32 = @truncate(@intFromPtr(&t));
    try std.testing.expectEqual(want, r.r32(dtc.off_vbr));
    try std.testing.expectEqual(want, r.r32(dtc.off_vbr_sec));
    try std.testing.expectEqual([2]u8{ 0x00, 0 }, r.w8[0]);
    try std.testing.expectEqual([2]u8{ 0x0C, 0 }, r.w8[1]);
    try std.testing.expectEqual(@as(usize, 1), o.info);
}

test "init rejects a null base and returns an MSTP failure with its step" {
    var r = Regs{};
    var o = Ops{};
    var s = dtc.State{};
    try std.testing.expectEqual(dtc.null_ptr, s.init(&r, &o, null));
    try std.testing.expectEqualStrings("vector_base must not be nullptr", o.err.?);
    var t = Table{};
    o = .{ .mstp_err = 0x201 };
    try std.testing.expectEqual(@as(u16, 0x201), s.init(&r, &o, &t));
    try std.testing.expectEqualStrings("dtc_init: mstp enable", o.err.?);
    try std.testing.expect(s.vector_base == null);
    try std.testing.expectEqual(@as(usize, 0), r.n8);
}

test "enable cleans the table first, and deinit clears everything" {
    var r = Regs{};
    var o = Ops{};
    var t = Table{};
    var s = dtc.State{};
    try std.testing.expectEqual(dtc.ok, s.enable(&r, &o));
    try std.testing.expectEqual(@as(usize, 0), o.nc);
    _ = s.init(&r, &o, &t);
    try std.testing.expectEqual(dtc.ok, s.enable(&r, &o));
    try std.testing.expectEqual(@intFromPtr(&t), o.cleans[0]);
    try std.testing.expectEqual(@as(u32, 1024), o.sizes[0]);
    try std.testing.expectEqual(dtc.st_start, r.mem[dtc.off_st]);
    try std.testing.expectEqual(@as(u16, 0x77), s.deinit(&r, &o));
    try std.testing.expect(s.vector_base == null and s.handler == null);
    try std.testing.expectEqual(@as(u32, 0), r.r32(dtc.off_vbr_sec));
    try std.testing.expectEqual(dtc.ok, dtc.disable(&r));
}

test "reconfigure swaps the table and toggles RRS" {
    var r = Regs{};
    var o = Ops{};
    var t = Table{};
    var s = dtc.State{};
    try std.testing.expectEqual(dtc.null_ptr, s.reconfigure(&r, &o, null));
    try std.testing.expectEqual(dtc.ok, s.reconfigure(&r, &o, &t));
    try std.testing.expectEqual([2]u8{ 0x0C, 0 }, r.w8[0]);
    try std.testing.expectEqual([2]u8{ 0x00, dtc.cr_rrs_disable }, r.w8[1]);
    try std.testing.expectEqual([2]u8{ 0x00, dtc.cr_rrs_enable }, r.w8[2]);
    try std.testing.expectEqual(@as(usize, 1), o.nc);
    try std.testing.expect(s.vector_base != null);
}

var seen_mask: u16 = 0;
var seen_ctx: ?*anyopaque = null;

fn handler(ctx: ?*anyopaque, mask: u16) callconv(.C) void {
    seen_mask = mask;
    seen_ctx = ctx;
}

test "status, clear_status and dispatch" {
    var r = Regs{};
    r.write16(dtc.off_sts, 0x8012);
    try std.testing.expectEqual(@as(u16, 0x8012), dtc.status(&r));
    try std.testing.expectEqual(dtc.ok, dtc.clearStatus(&r, 0x0002));
    try std.testing.expectEqual(@as(u16, 0x8010), dtc.status(&r));
    var s = dtc.State{};
    s.dispatch(&r);
    try std.testing.expectEqual(@as(u16, 0), dtc.status(&r));
    var token: u8 = 0;
    _ = s.attach(handler, &token);
    r.write16(dtc.off_sts, 0x8005);
    s.dispatch(&r);
    try std.testing.expectEqual(@as(u16, 0x8005), seen_mask);
    try std.testing.expectEqual(@as(?*anyopaque, &token), seen_ctx);
    try std.testing.expectEqual(@as(u16, 0), dtc.status(&r));
}

test "describe encodes a normal transfer" {
    var o = Ops{};
    var ti: dtc.Ti = undefined;
    const c = cfgOf(dtc.mode_normal, dtc.unit_word, 7, 0);
    try std.testing.expectEqual(dtc.ok, dtc.describe(&o, &c, &ti));
    try std.testing.expectEqual(@as(u32, 0x2800_0000), ti.mr);
    try std.testing.expectEqual(@as(u32, @truncate(@intFromPtr(&src_buf))), ti.sar);
    try std.testing.expectEqual(@as(u32, @truncate(@intFromPtr(&dst_buf))), ti.dar);
    try std.testing.expectEqual(@as(u16, 7), ti.cra);
    try std.testing.expectEqual(@as(u16, 0), ti.crb);
}

test "describe encodes block mode with 256 as zero" {
    var o = Ops{};
    var ti: dtc.Ti = undefined;
    var c = cfgOf(dtc.mode_block, dtc.unit_byte, 256, 3);
    c.dst_mode = dtc.addr_inc;
    try std.testing.expectEqual(dtc.ok, dtc.describe(&o, &c, &ti));
    try std.testing.expectEqual(@as(u32, 0x8808_0000), ti.mr);
    try std.testing.expectEqual(@as(u16, 0), ti.cra);
    try std.testing.expectEqual(@as(u16, 3), ti.crb);
    c.unit_count = 16;
    try std.testing.expectEqual(dtc.ok, dtc.describe(&o, &c, &ti));
    try std.testing.expectEqual(@as(u16, 0x1010), ti.cra);
}

test "describe rejects nulls, bad encodings and bad counts" {
    var o = Ops{};
    var ti: dtc.Ti = undefined;
    var c = cfgOf(dtc.mode_normal, dtc.unit_byte, 1, 0);
    try std.testing.expectEqual(dtc.null_ptr, dtc.describe(&o, null, &ti));
    try std.testing.expectEqual(dtc.null_ptr, dtc.describe(&o, &c, null));
    try std.testing.expectEqualStrings("out_ti must not be nullptr", o.err.?);
    c.src = null;
    try std.testing.expectEqual(dtc.null_ptr, dtc.describe(&o, &c, &ti));
    try std.testing.expectEqualStrings("cfg->src must not be nullptr", o.err.?);
    c = cfgOf(dtc.mode_normal, 3, 1, 0);
    try std.testing.expectEqual(dtc.invalid_arg, dtc.describe(&o, &c, &ti));
    c = cfgOf(1, dtc.unit_byte, 1, 0);
    try std.testing.expectEqual(dtc.invalid_arg, dtc.describe(&o, &c, &ti));
    c = cfgOf(dtc.mode_normal, dtc.unit_byte, 1, 1);
    try std.testing.expectEqual(dtc.invalid_arg, dtc.describe(&o, &c, &ti));
    try std.testing.expectEqualStrings("describe: normal mode wants a unit count and no block count", o.err.?);
    c = cfgOf(dtc.mode_block, dtc.unit_byte, 257, 1);
    try std.testing.expectEqual(dtc.invalid_arg, dtc.describe(&o, &c, &ti));
    try std.testing.expectEqualStrings("describe: block counts out of range", o.err.?);
    c = cfgOf(dtc.mode_block, dtc.unit_byte, 4, 0);
    try std.testing.expectEqual(dtc.invalid_arg, dtc.describe(&o, &c, &ti));
}

test "bind writes the slot, cleans TI then table and sets DTCE" {
    var r = Regs{};
    var o = Ops{};
    var t = Table{};
    var s = dtc.State{};
    var ti: dtc.Ti = undefined;
    const c = cfgOf(dtc.mode_normal, dtc.unit_half, 2, 0);
    try std.testing.expectEqual(dtc.invalid_state, s.bind(&o, 5, &c, &ti));
    try std.testing.expectEqualStrings("bind_activation: ra8_dtc_init has not run", o.err.?);
    _ = s.init(&r, &o, &t);
    try std.testing.expectEqual(dtc.invalid_arg, s.bind(&o, 96, &c, &ti));
    try std.testing.expectEqual(dtc.ok, s.bind(&o, 95, &c, &ti));
    try std.testing.expectEqual(@as(u32, @truncate(@intFromPtr(&ti))), t.entry[95]);
    try std.testing.expectEqual(@intFromPtr(&ti), o.cleans[0]);
    try std.testing.expectEqual(@as(u32, 16), o.sizes[0]);
    try std.testing.expectEqual(@intFromPtr(&t), o.cleans[1]);
    try std.testing.expectEqual(@as(?u16, 95), o.dtce);
    try std.testing.expectEqual(dtc.null_ptr, s.bind(&o, 1, null, &ti));
}

test "a failed describe in bind leaves the slot alone" {
    var r = Regs{};
    var o = Ops{};
    var t = Table{};
    var s = dtc.State{};
    var ti: dtc.Ti = undefined;
    _ = s.init(&r, &o, &t);
    const c = cfgOf(dtc.mode_normal, dtc.unit_byte, 0, 0);
    try std.testing.expectEqual(dtc.invalid_arg, s.bind(&o, 3, &c, &ti));
    try std.testing.expectEqual(@as(u32, 0), t.entry[3]);
    try std.testing.expectEqual(@as(?u16, null), o.dtce);
}

test "stop entry gates MSTP after stopping the engine" {
    var r = Regs{};
    var o = Ops{};
    r.mem[dtc.off_st] = 1;
    try std.testing.expectEqual(@as(u16, 0x77), dtc.enterStop(&r, &o));
    try std.testing.expectEqual(@as(u8, 0), r.mem[dtc.off_st]);
    try std.testing.expectEqual(@as(u16, 0), dtc.exitStop(&o));
    try std.testing.expectEqual(@as(usize, 1), o.enabled);
}
