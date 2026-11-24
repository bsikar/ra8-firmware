//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Shared SRAM layout, IPC channel mapping, and bidirectional RPC message
//! contract agreed between the Cortex-M85 and Cortex-M33 cores.

const std = @import("std");
const rpc = @import("ra8_rpc");
const ipc = rpc.ipc;
const Layout = rpc.ring.Layout;

pub const ctrl_addr: usize = ipc.shared_sram_base;
pub const ring_window_base: usize = ipc.shared_sram_base + 0x100;
pub const ring_data_bytes: usize = 256;
pub const ring_stride: usize = std.mem.alignForward(usize, Layout.header_bytes + ring_data_bytes, Layout.line);
pub const ring_count: usize = 4;
pub const ring_window_bytes: usize = ring_count * ring_stride;

pub const magic_ok: u32 = 0x5250_4331;
pub const poll_budget: u32 = 5_000_000;
pub const max_body: usize = 64;
pub const max_frame: usize = rpc.frame.maxSize(rpc.Envelope(max_body).Response);

pub const method_m33_mul_add: u16 = 1;
pub const method_m85_xor_sum: u16 = 2;
pub const topic_m33_notice: u16 = 0x10;
pub const topic_m85_notice: u16 = 0x20;
pub const pair0_caps: u32 = 0x11;
pub const pair1_caps: u32 = 0x22;

pub const CallArgs = struct {
    a: u32,
    b: u32,
};

pub const CallReply = struct {
    value: u32,
};

pub const EventMsg = struct {
    seq: u32,
    tag: u8,
};

pub const ClientT = rpc.Client(4, max_body);

/// Cooperative reset/readiness registers at `0x2210_0000` ahead of the rings.
pub const Control = extern struct {
    magic: u32,
    epoch: u32,
    cpu1_hold_req: u32,
    cpu1_hold_ack: u32,
    m33_ready: u32,
    m33_round1_calls: u32,
    m33_round2_calls: u32,
    m33_events_seen: u32,
    m33_failed: u32,
};

pub fn control() *volatile Control {
    return @ptrFromInt(ctrl_addr);
}

pub fn ringSlice(index: usize) []align(4) u8 {
    const addr = ring_window_base + index * ring_stride;
    const ptr: [*]align(4) u8 = @ptrFromInt(addr);
    return ptr[0 .. Layout.header_bytes + ring_data_bytes];
}

pub fn initChannel(core: ipc.Core, pair: ipc.Pair) ipc.Channel {
    const base_idx: usize = @as(usize, @backingInt(pair)) * 2;
    const tx_idx: usize = switch (core) {
        .cpu0 => if (pair == .pair0) base_idx else base_idx + 1,
        .cpu1 => if (pair == .pair0) base_idx + 1 else base_idx,
    };
    const rx_idx: usize = if (tx_idx == base_idx) base_idx + 1 else base_idx;
    return ipc.Channel.init(
        ringSlice(tx_idx),
        ringSlice(rx_idx),
        ipc.IpcSignal.init(core, pair, ipc.mmio),
    ) catch unreachable;
}
