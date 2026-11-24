//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Cortex-M33 half of `rpc_ring_dualcore`.
//!
//! Copies `.data`, clears `.bss`, waits for CPU0 to finish formatting the shared
//! rings (`epoch == 1`), runs Pair 0 as server (answering CPU0 calls and emitting
//! events) and Pair 1 as client (calling CPU0 and verifying events back), parks
//! on cooperative reset hold while CPU0 reformats both rings, calls
//! `RingTransport.reset()` only after `epoch == 2`, and completes Round 2.

const rpc = @import("ra8_rpc");
const ipc = rpc.ipc;
const shared = @import("shared.zig");

extern const g_ra8_ls_cpu1_stack_top: u8;
extern var g_ra8_ls_cpu1_data_start: u32;
extern var g_ra8_ls_cpu1_data_end: u32;
extern const g_ra8_ls_cpu1_data_load: u32;
extern var g_ra8_ls_cpu1_bss_start: u32;
extern var g_ra8_ls_cpu1_bss_end: u32;

const M33Service = struct {
    round_calls: u32 = 0,
    total_calls: u32 = 0,

    fn mulAdd(self: *M33Service, args: shared.CallArgs) rpc.Outcome(shared.CallReply) {
        self.round_calls += 1;
        self.total_calls += 1;
        return .{ .ok = .{ .value = args.a * 3 + args.b } };
    }
};

const M33ServerT = rpc.Server(M33Service, shared.max_body, .{.{ shared.method_m33_mul_add, M33Service.mulAdd }});

const M33Node = struct {
    p0: ipc.Channel = undefined,
    p1: ipc.Channel = undefined,
    svc: M33Service = .{},
    server_rx: [shared.max_frame]u8 = undefined,
    client_rx: [shared.max_frame]u8 = undefined,
    tx: [shared.max_frame]u8 = undefined,
    server: M33ServerT = undefined,
    client: shared.ClientT = undefined,

    fn bindChannels(self: *M33Node) void {
        self.p0 = shared.initChannel(.cpu1, .pair0);
        self.p1 = shared.initChannel(.cpu1, .pair1);
        self.p0.bind();
        self.p1.bind();
    }

    fn resetSessions(self: *M33Node) void {
        self.svc.round_calls = 0;
        self.server = M33ServerT.init(self.p0.transport(), &self.server_rx, &self.svc, shared.pair0_caps);
        self.client = shared.ClientT.init(self.p1.transport(), &self.client_rx, shared.pair1_caps);
    }

    fn pumpPair0(self: *M33Node, epoch: u32) !void {
        _ = self.p0.ipc.drainDoorbell();
        switch (try self.server.poll(&self.tx)) {
            .answered => {
                const seq: u32 = if (epoch == 1) 1 else 3;
                const tag: u8 = if (epoch == 1) 'A' else 'C';
                try self.server.emit(shared.EventMsg, shared.topic_m33_notice, .{ .seq = seq, .tag = tag }, &self.tx);
            },
            .greeted, .idle => {},
        }
    }
};

fn fault() callconv(.c) noreturn {
    shared.control().m33_failed = 1;
    while (true) asm volatile ("nop");
}

fn runRound(
    node: *M33Node,
    ctrl: *volatile shared.Control,
    epoch: u32,
    a: u32,
    b: u32,
    want_reply: u32,
    want_seq: u32,
    want_tag: u8,
) bool {
    var got_ready = false;
    var got_reply = false;
    var got_event = false;
    var sent_call = false;
    var call_id: u32 = 0;

    node.client.greet(&node.tx) catch return false;

    while (true) {
        if (ctrl.cpu1_hold_req != 0) break;
        node.pumpPair0(epoch) catch {};
        _ = node.p1.ipc.drainDoorbell();

        if (!got_reply or !got_event) {
            if (node.client.poll(&node.tx) catch return false) |inc| {
                switch (inc) {
                    .ready => got_ready = true,
                    .response => |resp| {
                        if (resp.id != call_id) return false;
                        const ok_bytes = switch (resp.result) {
                            .ok => |bytes| bytes,
                            .err => return false,
                        };
                        const reply = rpc.codec.decode(shared.CallReply, ok_bytes) catch return false;
                        if (reply.value != want_reply) return false;
                        got_reply = true;
                        if (epoch == 1) ctrl.m33_round1_calls += 1 else ctrl.m33_round2_calls += 1;
                    },
                    .event => |ev| {
                        if (ev.topic != shared.topic_m85_notice) return false;
                        const msg = rpc.codec.decode(shared.EventMsg, ev.payload) catch return false;
                        if (msg.seq != want_seq or msg.tag != want_tag) return false;
                        got_event = true;
                        ctrl.m33_events_seen += 1;
                    },
                }
            }
            if (got_ready and !sent_call) {
                call_id = node.client.call(shared.CallArgs, shared.method_m85_xor_sum, .{ .a = a, .b = b }, 2, &node.tx) catch return false;
                sent_call = true;
            }
        }
        if (got_reply and got_event and node.svc.round_calls >= 1) return true;
    }
    return false;
}

fn waitForHoldAndReset(node: *M33Node, ctrl: *volatile shared.Control, next_epoch: u32) bool {
    while (ctrl.cpu1_hold_req == 0) {
        node.pumpPair0(1) catch {};
        asm volatile ("nop");
    }
    ctrl.cpu1_hold_ack = 1;
    ipc.memoryBarrier();

    while (ctrl.epoch < next_epoch) {
        asm volatile ("nop");
    }
    node.p0.resetAfterReformat() catch return false;
    node.p1.resetAfterReformat() catch return false;
    node.resetSessions();
    ctrl.cpu1_hold_ack = 0;
    ipc.memoryBarrier();
    return true;
}

fn cpu1Main() noreturn {
    const ctrl = shared.control();
    while (ctrl.magic != shared.magic_ok or ctrl.epoch < 1) {
        asm volatile ("nop");
    }

    var node: M33Node = .{};
    node.bindChannels();
    node.p0.resetAfterReformat() catch {
        ctrl.m33_failed = 1;
        fault();
    };
    node.p1.resetAfterReformat() catch {
        ctrl.m33_failed = 1;
        fault();
    };
    node.resetSessions();
    ctrl.m33_ready = 1;
    ipc.memoryBarrier();

    if (!runRound(&node, ctrl, 1, 0xAA, 0x55, 0xFF, 2, 'B')) {
        ctrl.m33_failed = 1;
        fault();
    }
    if (!waitForHoldAndReset(&node, ctrl, 2)) {
        ctrl.m33_failed = 1;
        fault();
    }
    if (!runRound(&node, ctrl, 2, 0x1234, 0x00FF, 0x12CB, 4, 'D')) {
        ctrl.m33_failed = 1;
        fault();
    }
    while (true) {
        node.pumpPair0(2) catch {};
        asm volatile ("nop");
    }
}

export fn cpu1_reset_handler() callconv(.c) noreturn {
    var dst: [*]u32 = @ptrCast(&g_ra8_ls_cpu1_data_start);
    const dst_end: [*]u32 = @ptrCast(&g_ra8_ls_cpu1_data_end);
    var src: [*]const u32 = @ptrCast(&g_ra8_ls_cpu1_data_load);
    while (@intFromPtr(dst) < @intFromPtr(dst_end)) {
        dst[0] = src[0];
        dst += 1;
        src += 1;
    }
    var bss: [*]u32 = @ptrCast(&g_ra8_ls_cpu1_bss_start);
    const bss_end: [*]u32 = @ptrCast(&g_ra8_ls_cpu1_bss_end);
    while (@intFromPtr(bss) < @intFromPtr(bss_end)) {
        bss[0] = 0;
        bss += 1;
    }
    cpu1Main();
}

export const g_cpu1_vector_table: [8]*const anyopaque linksection(".cpu1_vectors") = .{
    &g_ra8_ls_cpu1_stack_top,
    &cpu1_reset_handler,
    &fault,
    &fault,
    &fault,
    &fault,
    &fault,
    &fault,
};
