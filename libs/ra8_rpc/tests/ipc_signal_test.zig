//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host unit tests for `ra8_rpc.ipc`: channel-pair routing, compiler+dmb
//! barrier ordering, hint-only 4-deep IPC FIFO doorbell coalescing, and
//! shared-SRAM / non-cacheable Armv8-M MPU validation.

const std = @import("std");
const testing = std.testing;

const rpc = @import("ra8_rpc");
const ipc = rpc.ipc;
const Layout = rpc.ring.Layout;

comptime {
    _ = @import("messages.zig");
    _ = @import("mock_queue.zig");
    _ = @import("mock_signal.zig");
    _ = @import("service.zig");
    _ = @import("fuzz_corpus.zig");
}

const FakeFifo = struct {
    words: [ipc.ChannelGeom.fifo_depth]u32 = @splat(0),
    rd: usize = 0,
    count: usize = 0,
    rerr: bool = false,
    ferr: bool = false,
    irq: u32 = 0,
    writes: u32 = 0,

    fn sta(self: *const FakeFifo) u32 {
        var val: u32 = self.irq & ipc.Bits.irq_mask;
        if (self.count > 0) val |= ipc.Bits.sta_rdy;
        if (self.count >= ipc.ChannelGeom.fifo_depth) val |= ipc.Bits.sta_full;
        if (self.rerr) val |= ipc.Bits.sta_rerr;
        if (self.ferr) val |= ipc.Bits.sta_ferr;
        return val;
    }

    fn writeTxd(self: *FakeFifo, word: u32) void {
        self.writes += 1;
        if (self.count >= ipc.ChannelGeom.fifo_depth) {
            self.ferr = true;
            return;
        }
        const wr = (self.rd + self.count) % ipc.ChannelGeom.fifo_depth;
        self.words[wr] = word;
        self.count += 1;
    }

    fn readRxd(self: *FakeFifo) u32 {
        if (self.count == 0) {
            self.rerr = true;
            return 0;
        }
        const out = self.words[self.rd];
        self.rd = (self.rd + 1) % ipc.ChannelGeom.fifo_depth;
        self.count -= 1;
        return out;
    }

    fn writeClr(self: *FakeFifo, mask: u32) void {
        if ((mask & ipc.Bits.clr_rst) != 0) {
            self.rd = 0;
            self.count = 0;
        }
        if ((mask & ipc.Bits.clr_rclr) != 0) self.rerr = false;
        if ((mask & ipc.Bits.clr_fclr) != 0) self.ferr = false;
        self.irq &= ~(mask & ipc.Bits.irq_mask);
    }
};

const FakeHardware = struct {
    channels: [ipc.ChannelGeom.count]FakeFifo = @splat(.{}),
    mpu_type: u32 = 8 << 8,
    mpu_ctrl: u32 = 1 | (1 << 2),
    mpu_rnr: u32 = 0,
    mpu_rbar: [8]u32 = @splat(0),
    mpu_rlar: [8]u32 = @splat(0),
    mpu_mair0: u32 = 0x0004_44FF,
    mpu_mair1: u32 = 0,

    fn initBootMpu(self: *FakeHardware) void {
        // Region 0..3: cacheable/device boot windows.
        // Region 4: shared SRAM2+3 (0x2210_0000..0x2219_FFFF), RW/XN, attrIdx 1 (0x44).
        self.mpu_rbar[4] = 0x2210_0000 | 0x03;
        self.mpu_rlar[4] = (0x2219_FFE0) | 1 | (1 << 1);
    }

    fn bus(self: *FakeHardware) ipc.Bus {
        return .{ .ctx = self, .vtable = &vtable };
    }

    const vtable: ipc.Bus.VTable = .{
        .read32 = read32,
        .write32 = write32,
    };

    fn from(ctx: *anyopaque) *FakeHardware {
        return @ptrCast(@alignCast(ctx));
    }

    fn read32(ctx: *anyopaque, addr: usize) u32 {
        const self = from(ctx);
        if (addr == ipc.MpuAddr.type_reg) return self.mpu_type;
        if (addr == ipc.MpuAddr.ctrl) return self.mpu_ctrl;
        if (addr == ipc.MpuAddr.rnr) return self.mpu_rnr;
        if (addr == ipc.MpuAddr.rbar) return self.mpu_rbar[@min(self.mpu_rnr, 7)];
        if (addr == ipc.MpuAddr.rlar) return self.mpu_rlar[@min(self.mpu_rnr, 7)];
        if (addr == ipc.MpuAddr.mair0) return self.mpu_mair0;
        if (addr == ipc.MpuAddr.mair1) return self.mpu_mair1;

        for (0..ipc.ChannelGeom.count) |ch| {
            const c: u8 = @intCast(ch);
            if (addr == ipc.channelRegAddr(ipc.base_addr, c, ipc.ChannelReg.sta)) return self.channels[ch].sta();
            if (addr == ipc.channelRegAddr(ipc.base_addr, c, ipc.ChannelReg.rxd)) return self.channels[ch].readRxd();
        }
        return 0;
    }

    fn write32(ctx: *anyopaque, addr: usize, value: u32) void {
        const self = from(ctx);
        if (addr == ipc.MpuAddr.rnr) {
            self.mpu_rnr = value;
            return;
        }
        if (addr == ipc.MpuAddr.rbar) {
            self.mpu_rbar[@min(self.mpu_rnr, 7)] = value;
            return;
        }
        if (addr == ipc.MpuAddr.rlar) {
            self.mpu_rlar[@min(self.mpu_rnr, 7)] = value;
            return;
        }
        for (0..ipc.ChannelGeom.count) |ch| {
            const c: u8 = @intCast(ch);
            if (addr == ipc.channelRegAddr(ipc.base_addr, c, ipc.ChannelReg.txd)) {
                self.channels[ch].writeTxd(value);
                return;
            }
            if (addr == ipc.channelRegAddr(ipc.base_addr, c, ipc.ChannelReg.clr)) {
                self.channels[ch].writeClr(value);
                return;
            }
        }
    }
};

const BarrierTrace = struct {
    mem: []const u8,
    heads: [8]u32 = undefined,
    tails: [8]u32 = undefined,
    count: usize = 0,

    fn hook(ctx: ?*anyopaque) void {
        const self: *BarrierTrace = @ptrCast(@alignCast(ctx.?));
        if (self.count < self.heads.len) {
            self.heads[self.count] = std.mem.readInt(u32, self.mem[Layout.At.head..][0..4], .little);
            self.tails[self.count] = std.mem.readInt(u32, self.mem[Layout.At.tail..][0..4], .little);
        }
        self.count += 1;
    }
};

test "core and pair routing matches RA8D2 IPC0/IPC1 channel convention" {
    try testing.expectEqual(@as(u8, 2), ipc.sendChannel(.cpu0, .pair0));
    try testing.expectEqual(@as(u8, 0), ipc.recvChannel(.cpu0, .pair0));
    try testing.expectEqual(@as(u8, 3), ipc.sendChannel(.cpu0, .pair1));
    try testing.expectEqual(@as(u8, 1), ipc.recvChannel(.cpu0, .pair1));

    try testing.expectEqual(@as(u8, 0), ipc.sendChannel(.cpu1, .pair0));
    try testing.expectEqual(@as(u8, 2), ipc.recvChannel(.cpu1, .pair0));
    try testing.expectEqual(@as(u8, 1), ipc.sendChannel(.cpu1, .pair1));
    try testing.expectEqual(@as(u8, 3), ipc.recvChannel(.cpu1, .pair1));

    try testing.expectEqual(@as(usize, 0x4002_00C0), ipc.channelRegAddr(ipc.base_addr, 0, ipc.ChannelReg.sta));
    try testing.expectEqual(@as(usize, 0x4002_0108), ipc.channelRegAddr(ipc.base_addr, 2, ipc.ChannelReg.txd));
}

test "barrier orders stores before index updates and notify coalesces when FIFO is full" {
    var hw: FakeHardware = .{};
    var tx_mem: [Layout.header_bytes + 64]u8 align(4) = undefined;
    var rx_mem: [Layout.header_bytes + 64]u8 align(4) = undefined;
    var trace: BarrierTrace = .{ .mem = &tx_mem };

    var sig = ipc.IpcSignal.init(.cpu0, .pair0, hw.bus());
    sig.barrier_hook = BarrierTrace.hook;
    sig.hook_ctx = &trace;

    var ch = try ipc.Channel.init(&tx_mem, &rx_mem, sig);
    ch.bind();
    try ch.formatRings(true);

    // Send 6 small frames without the peer draining the 4-stage TX FIFO.
    // Four doorbells enter FIFO10 (channel 2); the next two coalesce cleanly
    // without triggering FERR or dropping any bytes from the shared ring.
    for (0..6) |_| try ch.transport().send("ok");
    try testing.expectEqual(@as(u32, 4), sig_sent(&ch));
    try testing.expectEqual(@as(u32, 2), ch.ipc.doorbells_coalesced);
    try testing.expectEqual(@as(u32, 4), hw.channels[2].count);
    try testing.expect(!hw.channels[2].ferr);

    // Every barrier fired before head moved to its new value on the first write.
    try testing.expectEqual(@as(usize, 6), trace.count);
    try testing.expectEqual(@as(u32, 0), trace.heads[0]);
    try testing.expectEqual(@as(u32, 2), trace.heads[1]);

    // Peer side drains channel 2 and reads all 12 ring bytes intact.
    const peer_sig = ipc.IpcSignal.init(.cpu1, .pair0, hw.bus());
    var peer_ch = try ipc.Channel.init(&rx_mem, &tx_mem, peer_sig);
    peer_ch.bind();
    try testing.expect(peer_ch.ipc.hasDoorbell());
    try testing.expectEqual(@as(u32, 4), peer_ch.ipc.drainDoorbell());
    try testing.expect(!peer_ch.ipc.hasDoorbell());

    var buf: [16]u8 = undefined;
    const n = try peer_ch.transport().receive(&buf);
    try testing.expectEqual(@as(usize, 12), n);
    try testing.expectEqualStrings("okokokokokok", buf[0..n]);
}

fn sig_sent(ch: *const ipc.Channel) u32 {
    return ch.ipc.doorbells_sent;
}

test "notify clears pre-existing FERR and drainDoorbell clears RERR and IRQ bits" {
    var hw: FakeHardware = .{};
    hw.channels[3].ferr = true;
    hw.channels[1].rerr = true;
    hw.channels[1].irq = 0x5;

    var sig = ipc.IpcSignal.init(.cpu0, .pair1, hw.bus());
    sig.signal().notify();
    try testing.expect(!hw.channels[3].ferr);
    try testing.expectEqual(@as(u32, 1), sig.doorbells_sent);

    hw.channels[1].writeTxd(99);
    try testing.expectEqual(@as(u32, 1), sig.drainDoorbell());
    try testing.expect(!hw.channels[1].rerr);
    try testing.expectEqual(@as(u32, 0), hw.channels[1].irq);
}

test "shared SRAM window bounds and carvePair preserve 32-byte line separation" {
    try ipc.validateSharedWindow(ipc.shared_sram_base, 512);
    try ipc.validateSharedWindow(ipc.shared_sram_base + ipc.shared_sram_size - 512, 512);

    try testing.expectError(error.UnalignedAddress, ipc.validateSharedWindow(ipc.shared_sram_base + 2, 512));
    try testing.expectError(error.WindowTooSmall, ipc.validateSharedWindow(ipc.shared_sram_base, 16));
    try testing.expectError(error.OutsideSharedSram, ipc.validateSharedWindow(0x220F_FFE0, 512));
    try testing.expectError(
        error.OutsideSharedSram,
        ipc.validateSharedWindow(ipc.shared_sram_base + ipc.shared_sram_size - 64, 512),
    );
    try testing.expectError(
        error.OutsideSharedSram,
        ipc.validateSharedWindow((std.math.maxInt(usize) - 15), 512),
    );

    var raw: [512]u8 align(4) = undefined;
    const split = try ipc.carvePair(&raw);
    try testing.expect(@intFromPtr(split.rx.ptr) - @intFromPtr(split.tx.ptr) == 256);
    try testing.expect(@intFromPtr(split.tx.ptr) + split.tx.len == @intFromPtr(split.rx.ptr));
}

test "verifyNonCacheableMpu accepts MAIR 0x44 RW region and rejects cacheable or RO maps" {
    var hw: FakeHardware = .{};
    hw.initBootMpu();
    try ipc.verifyNonCacheableMpu(hw.bus(), ipc.shared_sram_base, 4096);

    // Cacheable WB/WA attribute (AttrIdx 0 = 0xFF) must be rejected.
    hw.mpu_rlar[4] = (0x2219_FFE0) | 1 | (0 << 1);
    try testing.expectError(error.CacheableOrUnmapped, ipc.verifyNonCacheableMpu(hw.bus(), ipc.shared_sram_base, 4096));

    // Read-only permission (AP = 0b10) must be rejected.
    hw.mpu_rbar[4] = 0x2210_0000 | (2 << 1);
    hw.mpu_rlar[4] = (0x2219_FFE0) | 1 | (1 << 1);
    try testing.expectError(error.ReadOnly, ipc.verifyNonCacheableMpu(hw.bus(), ipc.shared_sram_base, 4096));

    // Disabled MPU must be rejected.
    hw.initBootMpu();
    hw.mpu_ctrl = 0;
    try testing.expectError(error.MpuDisabled, ipc.verifyNonCacheableMpu(hw.bus(), ipc.shared_sram_base, 4096));

    // Zero regions must be rejected.
    hw.mpu_ctrl = 1;
    hw.mpu_type = 0;
    try testing.expectError(error.NoRegions, ipc.verifyNonCacheableMpu(hw.bus(), ipc.shared_sram_base, 4096));
}
