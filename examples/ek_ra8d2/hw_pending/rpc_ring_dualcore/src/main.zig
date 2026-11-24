//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Cortex-M85 half of `rpc_ring_dualcore`.
//!
//! Verifies that boot setup mapped shared SRAM (`0x2210_0000`) Normal
//! Non-cacheable under an active MPU/D-cache, formats all shared rings before
//! releasing CPU1, exchanges bidirectional calls and events with CPU1 across
//! both IPC FIFO channel pairs, forces a ring header fault, holds CPU1 while
//! reformatting both rings (`Ring.writeHeader`), resets both `RingTransport`s,
//! and completes a post-reset bidirectional exchange.

const std = @import("std");
const rpc = @import("ra8_rpc");
const ipc = rpc.ipc;
const shared = @import("shared.zig");

pub const panic = std.debug.no_panic;

const baud: u32 = 115_200;
const ok: c_int = 0;

const pass_line = "rpc_ring_dualcore: bidirectional M85<->M33 RPC and reset PASS\r\n";
const fail_line = "rpc_ring_dualcore: FAIL\r\n";

export var g_rpc_dualcore_pass: u32 = 0;

extern fn ra8_cgc_init() c_int;
extern fn ra8_board_uart_console_init(baud: u32) c_int;
extern fn ra8_board_uart_console_write(data: [*]const u8, len: usize) c_int;
extern fn ra8_board_uart_console_flush() c_int;
extern fn ra8_cpu1_release(mram: *const anyopaque, stack: *const anyopaque) c_int;
extern const g_ra8_ls_cpu1_mram_start: u8;
extern const g_ra8_ls_cpu1_stack_top: u8;

const M85Service = struct {
    calls: u32 = 0,

    fn xorSum(self: *M85Service, args: shared.CallArgs) rpc.Outcome(shared.CallReply) {
        self.calls += 1;
        return .{ .ok = .{ .value = args.a ^ args.b } };
    }
};

const M85ServerT = rpc.Server(M85Service, shared.max_body, .{.{ shared.method_m85_xor_sum, M85Service.xorSum }});

const M85Node = struct {
    p0: ipc.Channel = undefined,
    p1: ipc.Channel = undefined,
    svc: M85Service = .{},
    client_rx: [shared.max_frame]u8 = undefined,
    server_rx: [shared.max_frame]u8 = undefined,
    tx: [shared.max_frame]u8 = undefined,
    client: shared.ClientT = undefined,
    server: M85ServerT = undefined,

    fn bindChannels(self: *M85Node) void {
        self.p0 = shared.initChannel(.cpu0, .pair0);
        self.p1 = shared.initChannel(.cpu0, .pair1);
        self.p0.bind();
        self.p1.bind();
    }

    fn resetSessions(self: *M85Node) void {
        self.client = shared.ClientT.init(self.p0.transport(), &self.client_rx, shared.pair0_caps);
        self.server = M85ServerT.init(self.p1.transport(), &self.server_rx, &self.svc, shared.pair1_caps);
    }

    fn pumpIncoming(self: *M85Node) !void {
        _ = self.p1.ipc.drainDoorbell();
        switch (try self.server.poll(&self.tx)) {
            .answered => {
                const seq: u32 = if (self.svc.calls == 1) 2 else 4;
                const tag: u8 = if (self.svc.calls == 1) 'B' else 'D';
                try self.server.emit(shared.EventMsg, shared.topic_m85_notice, .{ .seq = seq, .tag = tag }, &self.tx);
            },
            .greeted, .idle => {},
        }
    }
};

fn say(line: []const u8) void {
    _ = ra8_board_uart_console_write(line.ptr, line.len);
    _ = ra8_board_uart_console_flush();
}

fn park() noreturn {
    while (true) asm volatile ("wfi");
}

fn waitWord(ptr: *const volatile u32, want: u32) bool {
    var i: u32 = 0;
    while (i < shared.poll_budget) : (i += 1) {
        if (ptr.* == want) return true;
    }
    return false;
}

fn roundExchange(node: *M85Node, a: u32, b: u32, want_reply: u32, want_seq: u32, want_tag: u8) bool {
    var got_ready = false;
    var got_reply = false;
    var got_event = false;
    var sent_call = false;
    var call_id: u32 = 0;

    node.client.greet(&node.tx) catch return false;

    var i: u32 = 0;
    while (i < shared.poll_budget) : (i += 1) {
        node.pumpIncoming() catch return false;
        _ = node.p0.ipc.drainDoorbell();
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
                },
                .event => |ev| {
                    if (ev.topic != shared.topic_m33_notice) return false;
                    const msg = rpc.codec.decode(shared.EventMsg, ev.payload) catch return false;
                    if (msg.seq != want_seq or msg.tag != want_tag) return false;
                    got_event = true;
                },
            }
        }
        if (got_ready and !sent_call) {
            call_id = node.client.call(shared.CallArgs, shared.method_m33_mul_add, .{ .a = a, .b = b }, 1, &node.tx) catch return false;
            sent_call = true;
        }
        if (got_reply and got_event and node.svc.calls >= want_seq / 2 + 1) break;
    }
    return got_reply and got_event;
}

fn waitCpu1Events(node: *M85Node, ctrl: *volatile shared.Control, want_events: u32) bool {
    var i: u32 = 0;
    while (i < shared.poll_budget) : (i += 1) {
        node.pumpIncoming() catch return false;
        if (ctrl.m33_events_seen >= want_events and ctrl.m33_failed == 0) return true;
        if (ctrl.m33_failed != 0) return false;
    }
    return false;
}

fn resetBothSafely(node: *M85Node, ctrl: *volatile shared.Control) bool {
    // Fault Pair 0 header so RingTransport latches BadMessage.
    std.mem.writeInt(u32, shared.ringSlice(0)[0..4], 0xDEAD_BEEF, .little);
    if (node.client.call(shared.CallArgs, shared.method_m33_mul_add, .{ .a = 1, .b = 1 }, 9, &node.tx)) |_| {
        return false;
    } else |err| if (err != error.BadMessage) return false;

    // Hold CPU1 cooperatively while M85 formats both ring pairs via writeHeader().
    ctrl.cpu1_hold_req = 1;
    ipc.memoryBarrier();
    if (!waitWord(&ctrl.cpu1_hold_ack, 1)) return false;

    // Resetting before writeHeader() must be rejected.
    if (node.p0.resetAfterReformat()) |_| return false else |err| if (err != error.HeadersNotRewritten) return false;

    node.p0.formatRings(true) catch return false;
    node.p1.formatRings(true) catch return false;
    node.p0.resetAfterReformat() catch return false;
    node.p1.resetAfterReformat() catch return false;
    node.resetSessions();

    ctrl.cpu1_hold_req = 0;
    ipc.memoryBarrier();
    ctrl.epoch = 2;
    ipc.memoryBarrier();
    return waitWord(&ctrl.cpu1_hold_ack, 0);
}

fn run(ctrl: *volatile shared.Control) bool {
    ipc.verifyNonCacheableMpu(ipc.mmio, shared.ring_window_base, shared.ring_window_bytes) catch return false;

    var node: M85Node = .{};
    node.bindChannels();
    ctrl.* = .{
        .magic = shared.magic_ok,
        .epoch = 0,
        .cpu1_hold_req = 0,
        .cpu1_hold_ack = 0,
        .m33_ready = 0,
        .m33_round1_calls = 0,
        .m33_round2_calls = 0,
        .m33_events_seen = 0,
        .m33_failed = 0,
    };
    node.p0.formatRings(true) catch return false;
    node.p1.formatRings(true) catch return false;
    node.p0.resetAfterReformat() catch return false;
    node.p1.resetAfterReformat() catch return false;
    node.resetSessions();
    ctrl.epoch = 1;
    ipc.memoryBarrier();

    if (ra8_cpu1_release(&g_ra8_ls_cpu1_mram_start, &g_ra8_ls_cpu1_stack_top) != ok) return false;
    if (!waitWord(&ctrl.m33_ready, 1)) return false;

    if (!roundExchange(&node, 7, 5, 26, 1, 'A')) return false;
    if (!waitCpu1Events(&node, ctrl, 1)) return false;
    if (!resetBothSafely(&node, ctrl)) return false;
    if (!roundExchange(&node, 10, 3, 33, 3, 'C')) return false;
    if (!waitCpu1Events(&node, ctrl, 2)) return false;
    return node.svc.calls == 2 and ctrl.m33_round1_calls == 1 and ctrl.m33_round2_calls == 1;
}

export fn main() callconv(.c) c_int {
    if (ra8_cgc_init() != ok) park();
    const console = ra8_board_uart_console_init(baud) == ok;
    const passed = run(shared.control());
    if (passed) {
        const flag: *volatile u32 = &g_rpc_dualcore_pass;
        flag.* = 1;
    }
    if (console) say(if (passed) pass_line else fail_line);
    park();
}
