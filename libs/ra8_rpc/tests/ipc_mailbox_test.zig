//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host session tests for bidirectional M85 <-> M33 `ra8_rpc` communication over
//! `ipc.Channel`, covering calls in both directions, events in both
//! directions, FIFO coalescing under burst traffic, and coordinated reset.

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

    fn sta(self: *const FakeFifo) u32 {
        var val: u32 = 0;
        if (self.count > 0) val |= ipc.Bits.sta_rdy;
        if (self.count >= ipc.ChannelGeom.fifo_depth) val |= ipc.Bits.sta_full;
        return val;
    }

    fn writeTxd(self: *FakeFifo, word: u32) void {
        if (self.count >= ipc.ChannelGeom.fifo_depth) return;
        const wr = (self.rd + self.count) % ipc.ChannelGeom.fifo_depth;
        self.words[wr] = word;
        self.count += 1;
    }

    fn readRxd(self: *FakeFifo) u32 {
        if (self.count == 0) return 0;
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
    }
};

const FakeIpcBus = struct {
    channels: [ipc.ChannelGeom.count]FakeFifo = @splat(.{}),

    fn bus(self: *FakeIpcBus) ipc.Bus {
        return .{ .ctx = self, .vtable = &vtable };
    }

    const vtable: ipc.Bus.VTable = .{
        .read32 = read32,
        .write32 = write32,
    };

    fn from(ctx: *anyopaque) *FakeIpcBus {
        return @ptrCast(@alignCast(ctx));
    }

    fn read32(ctx: *anyopaque, addr: usize) u32 {
        const self = from(ctx);
        for (0..ipc.ChannelGeom.count) |ch| {
            const c: u8 = @intCast(ch);
            if (addr == ipc.channelRegAddr(ipc.base_addr, c, ipc.ChannelReg.sta)) return self.channels[ch].sta();
            if (addr == ipc.channelRegAddr(ipc.base_addr, c, ipc.ChannelReg.rxd)) return self.channels[ch].readRxd();
        }
        return 0;
    }

    fn write32(ctx: *anyopaque, addr: usize, value: u32) void {
        const self = from(ctx);
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

const CallArgs = struct {
    a: u32,
    b: u32,
};

const CallReply = struct {
    value: u32,
};

const EventMsg = struct {
    seq: u32,
    tag: u8,
};

const max_body = 64;
const max_frame = rpc.frame.maxSize(rpc.Envelope(max_body).Response);

const M33Service = struct {
    calls: u32 = 0,
    fn transform(self: *M33Service, args: CallArgs) rpc.Outcome(CallReply) {
        self.calls += 1;
        return .{ .ok = .{ .value = args.a * 3 + args.b } };
    }
};

const M85Service = struct {
    calls: u32 = 0,
    fn checksum(self: *M85Service, args: CallArgs) rpc.Outcome(CallReply) {
        self.calls += 1;
        return .{ .ok = .{ .value = args.a ^ args.b } };
    }
};

const ClientT = rpc.Client(4, max_body);
const M33ServerT = rpc.Server(M33Service, max_body, .{.{ 1, M33Service.transform }});
const M85ServerT = rpc.Server(M85Service, max_body, .{.{ 2, M85Service.checksum }});

const DualRig = struct {
    shared: [4 * (Layout.header_bytes + 128)]u8 align(4) = undefined,
    ipc_bus: FakeIpcBus = .{},

    m85_p0: ipc.Channel = undefined,
    m33_p0: ipc.Channel = undefined,
    m33_p1: ipc.Channel = undefined,
    m85_p1: ipc.Channel = undefined,

    m33_svc: M33Service = .{},
    m85_svc: M85Service = .{},

    m85_client_rx: [max_frame]u8 = undefined,
    m33_server_rx: [max_frame]u8 = undefined,
    m33_client_rx: [max_frame]u8 = undefined,
    m85_server_rx: [max_frame]u8 = undefined,
    tx: [max_frame]u8 = undefined,

    m85_client: ClientT = undefined,
    m33_server: M33ServerT = undefined,
    m33_client: ClientT = undefined,
    m85_server: M85ServerT = undefined,

    fn ringSlice(self: *DualRig, index: usize) []align(4) u8 {
        const span = Layout.header_bytes + 128;
        const off = index * span;
        return @alignCast(self.shared[off..][0..span]);
    }

    fn init(self: *DualRig) !void {
        self.* = .{};
        const bus = self.ipc_bus.bus();

        self.m85_p0 = try ipc.Channel.init(self.ringSlice(0), self.ringSlice(1), ipc.IpcSignal.init(.cpu0, .pair0, bus));
        self.m33_p0 = try ipc.Channel.init(self.ringSlice(1), self.ringSlice(0), ipc.IpcSignal.init(.cpu1, .pair0, bus));
        self.m33_p1 = try ipc.Channel.init(self.ringSlice(2), self.ringSlice(3), ipc.IpcSignal.init(.cpu1, .pair1, bus));
        self.m85_p1 = try ipc.Channel.init(self.ringSlice(3), self.ringSlice(2), ipc.IpcSignal.init(.cpu0, .pair1, bus));

        self.m85_p0.bind();
        self.m33_p0.bind();
        self.m33_p1.bind();
        self.m85_p1.bind();

        try self.m85_p0.formatRings(true);
        try self.m85_p1.formatRings(true);
        self.startSessions();
    }

    fn startSessions(self: *DualRig) void {
        self.m85_client = ClientT.init(self.m85_p0.transport(), &self.m85_client_rx, 1);
        self.m33_server = M33ServerT.init(self.m33_p0.transport(), &self.m33_server_rx, &self.m33_svc, 1);
        self.m33_client = ClientT.init(self.m33_p1.transport(), &self.m33_client_rx, 2);
        self.m85_server = M85ServerT.init(self.m85_p1.transport(), &self.m85_server_rx, &self.m85_svc, 2);
    }

    fn openBoth(self: *DualRig) !void {
        try self.m85_client.greet(&self.tx);
        try testing.expectEqual(rpc.Step.greeted, try self.m33_server.poll(&self.tx));
        try testing.expectEqual(@as(u32, 1), (try self.m85_client.poll(&self.tx)).?.ready);

        try self.m33_client.greet(&self.tx);
        try testing.expectEqual(rpc.Step.greeted, try self.m85_server.poll(&self.tx));
        try testing.expectEqual(@as(u32, 2), (try self.m33_client.poll(&self.tx)).?.ready);
    }
};

fn expectReply(client: *ClientT, tx: []u8, expected_id: u32, expected_waiter: usize, expected_val: u32) !void {
    const incoming = (try client.poll(tx)).?;
    try testing.expectEqual(expected_id, incoming.response.id);
    try testing.expectEqual(expected_waiter, incoming.response.waiter);
    const decoded = try rpc.codec.decode(CallReply, incoming.response.result.ok);
    try testing.expectEqual(expected_val, decoded.value);
}

fn expectEvent(client: *ClientT, tx: []u8, topic: u16, seq: u32, tag: u8) !void {
    const incoming = (try client.poll(tx)).?;
    try testing.expectEqual(topic, incoming.event.topic);
    const decoded = try rpc.codec.decode(EventMsg, incoming.event.payload);
    try testing.expectEqual(seq, decoded.seq);
    try testing.expectEqual(tag, decoded.tag);
}

test "bidirectional M85 <-> M33 calls and events cross both IPC channel pairs" {
    var rig: DualRig = undefined;
    try rig.init();
    try rig.openBoth();

    // Pair 0: M85 client calls M33 server and receives M33 -> M85 event.
    const id0 = try rig.m85_client.call(CallArgs, 1, .{ .a = 7, .b = 5 }, 11, &rig.tx);
    try testing.expectEqual(rpc.Step.answered, try rig.m33_server.poll(&rig.tx));
    try rig.m33_server.emit(EventMsg, 0x10, .{ .seq = 41, .tag = 'A' }, &rig.tx);
    try expectReply(&rig.m85_client, &rig.tx, id0, 11, 26);
    try expectEvent(&rig.m85_client, &rig.tx, 0x10, 41, 'A');

    // Pair 1: M33 client calls M85 server and receives M85 -> M33 event.
    const id1 = try rig.m33_client.call(CallArgs, 2, .{ .a = 0xAA, .b = 0x55 }, 22, &rig.tx);
    try testing.expectEqual(rpc.Step.answered, try rig.m85_server.poll(&rig.tx));
    try rig.m85_server.emit(EventMsg, 0x20, .{ .seq = 42, .tag = 'B' }, &rig.tx);
    try expectReply(&rig.m33_client, &rig.tx, id1, 22, 0xFF);
    try expectEvent(&rig.m33_client, &rig.tx, 0x20, 42, 'B');

    try testing.expectEqual(@as(u32, 1), rig.m33_svc.calls);
    try testing.expectEqual(@as(u32, 1), rig.m85_svc.calls);
}

test "burst exceeding 4-deep IPC FIFO delivers every event across the ring" {
    var rig: DualRig = undefined;
    try rig.init();
    try rig.openBoth();

    // Emit 5 events in a burst while ring has room: FIFO drops/coalesces the 5th
    // doorbell cleanly, yet all 5 frames arrive intact over Pair 0.
    for (0..5) |i| {
        try rig.m33_server.emit(EventMsg, 0x30, .{ .seq = @intCast(i), .tag = 'E' }, &rig.tx);
    }
    try testing.expect(rig.m33_p0.ipc.doorbells_coalesced > 0);
    _ = rig.m85_p0.ipc.drainDoorbell();

    for (0..5) |i| {
        try expectEvent(&rig.m85_client, &rig.tx, 0x30, @intCast(i), 'E');
    }
    try testing.expectEqual(@as(?ClientT.Incoming, null), try rig.m85_client.poll(&rig.tx));
}

test "coordinated reset enforces writeHeader while peer is held before RingTransport.reset" {
    var rig: DualRig = undefined;
    try rig.init();
    try rig.openBoth();

    // Corrupt ring 0 header to force sticky BadMessage on both endpoints.
    std.mem.writeInt(u32, rig.ringSlice(0)[0..4], 0xDEAD_BEEF, .little);
    try testing.expectError(error.BadMessage, rig.m85_client.call(CallArgs, 1, .{ .a = 1, .b = 2 }, 1, &rig.tx));
    try testing.expectError(error.BadMessage, rig.m33_server.poll(&rig.tx));

    // Refuses reset if peer is not held, and refuses transport reset before rewrite.
    try testing.expectError(error.PeerNotHeld, rig.m85_p0.formatRings(false));
    try testing.expectError(error.HeadersNotRewritten, rig.m85_p0.resetAfterReformat());
    try testing.expectError(error.HeadersNotRewritten, rig.m33_p0.resetAfterReformat());

    // Hold peer, rewrite both headers with Ring.writeHeader(), then both reset.
    try rig.m85_p0.formatRings(true);
    try rig.m85_p0.resetAfterReformat();
    try rig.m33_p0.resetAfterReformat();
    try rig.m85_p1.formatRings(true);
    try rig.m85_p1.resetAfterReformat();
    try rig.m33_p1.resetAfterReformat();

    rig.startSessions();
    try rig.openBoth();

    const id = try rig.m85_client.call(CallArgs, 1, .{ .a = 9, .b = 4 }, 99, &rig.tx);
    try testing.expectEqual(rpc.Step.answered, try rig.m33_server.poll(&rig.tx));
    try expectReply(&rig.m85_client, &rig.tx, id, 99, 31);
}
