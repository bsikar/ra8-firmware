//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! SCI DMA request building and TXI/RXI dispatch (RA8FW-592). Pure logic:
//! register access goes through an ops value with readCcr0/writeCcr0,
//! readRdr and writeTdr. HUM Ch 38.2.2 RDR p 2180, 38.2.3 TDR p 2181,
//! 38.2.5 CCR0 p 2182. Mirrors FSP r_sci_b_uart txi_isr/rxi_isr.

/// Byte transfers (`k_ra8_dmac_width_byte`, DMTMD.SZ = 00b).
pub const dma_width_byte: u8 = 0;
/// CCR0.RIE, receive interrupt enable.
pub const ccr0_rie: u32 = 1 << 16;
/// CCR0.TIE, transmit interrupt enable.
pub const ccr0_tie: u32 = 1 << 20;
/// RDR.RDAT[7:0] for 8-bit reception.
pub const rdr_mask_data8: u32 = 0xFF;
/// Highest SCI channel index (SCI0..SCI9).
pub const channel_max: u8 = 9;

pub const RxFn = *const fn (ctx: ?*anyopaque, byte: u8) callconv(.C) void;
pub const TxFn = *const fn (ctx: ?*anyopaque, byte: *u8) callconv(.C) bool;
pub const DoneFn = *const fn (ctx: ?*anyopaque) callconv(.C) void;

/// `ra8_sci_state_t`; storage stays in ra8_sci.c.
pub const State = extern struct {
    rx_fn: ?RxFn = null,
    rx_ctx: ?*anyopaque = null,
    tx_fn: ?TxFn = null,
    tx_ctx: ?*anyopaque = null,
    initialized: bool = false,
    tx_buf: ?[*]const u8 = null,
    tx_len: u32 = 0,
    tx_idx: u32 = 0,
    rx_buf: ?[*]u8 = null,
    rx_len: u32 = 0,
    rx_idx: u32 = 0,
};

/// `ra8_dma_request_t`.
pub const DmaRequest = extern struct {
    src_addr: usize,
    dst_addr: usize,
    count: u16,
    width: u8,
    src_inc: bool,
    dst_inc: bool,
    trigger: u16,
    on_complete: ?DoneFn,
    ctx: ?*anyopaque,
};

/// Software-start byte-stream request; ELC trigger routing is a later task.
pub fn makeRequest(src: usize, dst: usize, len: u16, src_inc: bool, dst_inc: bool, done: ?DoneFn, ctx: ?*anyopaque) DmaRequest {
    return .{
        .src_addr = src,
        .dst_addr = dst,
        .count = len,
        .width = dma_width_byte,
        .src_inc = src_inc,
        .dst_inc = dst_inc,
        .trigger = 0,
        .on_complete = done,
        .ctx = ctx,
    };
}

/// True when the channel exists and the length is non-zero.
pub fn argsOk(channel: u8, len: u16) bool {
    return channel <= channel_max and len != 0;
}

fn clearTie(regs: anytype) void {
    regs.writeCcr0(regs.readCcr0() & ~ccr0_tie);
}

/// TXI: push the next async byte (clearing TIE when drained), else ask the
/// attached handler for one.
pub fn dispatchTxi(regs: anytype, st: *State) void {
    if (st.tx_len > 0) {
        if (st.tx_idx < st.tx_len) {
            const byte = st.tx_buf.?[st.tx_idx];
            st.tx_idx += 1;
            regs.writeTdr(byte);
            if (st.tx_fn) |cb| {
                var echo = byte;
                _ = cb(st.tx_ctx, &echo);
            }
        }
        if (st.tx_idx >= st.tx_len) {
            clearTie(regs);
            st.tx_buf = null;
            st.tx_len = 0;
            st.tx_idx = 0;
        }
        return;
    }
    const cb = st.tx_fn orelse return clearTie(regs);
    var byte: u8 = 0;
    if (cb(st.tx_ctx, &byte)) regs.writeTdr(byte) else clearTie(regs);
}

/// RXI: append to the async buffer (clearing RIE when full), then hand the
/// byte to the attached handler either way.
pub fn dispatchRxi(regs: anytype, st: *State) void {
    const b: u8 = @truncate(regs.readRdr() & rdr_mask_data8);
    if (st.rx_len > 0) {
        if (st.rx_idx < st.rx_len) {
            st.rx_buf.?[st.rx_idx] = b;
            st.rx_idx += 1;
        }
        if (st.rx_idx >= st.rx_len) {
            regs.writeCcr0(regs.readCcr0() & ~ccr0_rie);
            st.rx_buf = null;
            st.rx_len = 0;
            st.rx_idx = 0;
        }
    }
    if (st.rx_fn) |cb| cb(st.rx_ctx, b);
}
