//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for SCI DMA TX/RX and the TXI/RXI/ERI dispatchers (RA8FW-592).
//! s_sci_state stays defined in ra8_sci.c; ra8_dma_request stays in C and
//! ra8_sci_clear_errors is in sci_ctl_abi.zig. Logic is in internal/sci_dma_isr.zig.

const common = @import("abi_common.zig");
const sd = @import("internal/sci_dma_isr.zig");

const tag = "SCI";
const sci0_base: usize = 0x4035_8000;
const channel_stride: usize = 0x100;
const off_rdr: usize = 0x00;
const off_tdr: usize = 0x04;
const off_ccr0: usize = 0x08;

extern var s_sci_state: [@as(usize, sd.channel_max) + 1]sd.State;
extern fn ra8_dma_request(req: *const sd.DmaRequest, out_channel: *u8) u16;
extern fn ra8_sci_clear_errors(channel: u8) u16;

comptime {
    if (@sizeOf(sd.DmaRequest) != 40 and @sizeOf(usize) == 8) @compileError("ra8_dma_request_t is 40 bytes on host");
    if (@offsetOf(sd.DmaRequest, "trigger") != 22 and @sizeOf(usize) == 8) @compileError("trigger sits at +22");
    if (@sizeOf(sd.State) != 72 and @sizeOf(usize) == 8) @compileError("ra8_sci_state_t is 72 bytes on host");
    if (@offsetOf(sd.State, "rx_idx") != 68 and @sizeOf(usize) == 8) @compileError("rx_idx sits at +68");
}

const Mmio = struct {
    base: usize,
    fn reg(self: Mmio, off: usize) *volatile u32 {
        return @ptrFromInt(self.base + off);
    }
    pub fn readCcr0(self: Mmio) u32 {
        return self.reg(off_ccr0).*;
    }
    pub fn writeCcr0(self: Mmio, v: u32) void {
        self.reg(off_ccr0).* = v;
    }
    pub fn readRdr(self: Mmio) u32 {
        return self.reg(off_rdr).*;
    }
    pub fn writeTdr(self: Mmio, b: u8) void {
        self.reg(off_tdr).* = b;
    }
};

fn mmio(channel: u8) Mmio {
    return .{ .base = sci0_base + @as(usize, channel) * channel_stride };
}

fn nullPtr(msg: [*:0]const u8) u16 {
    common.ra8_log_emit_error(tag, msg);
    return common.k_ra8_err_null_ptr;
}

export fn ra8_sci_write_dma(channel: u8, data: ?[*]const u8, len: u16, done: ?sd.DoneFn, ctx: ?*anyopaque, out: ?*u8) u16 {
    const src = data orelse return nullPtr("write_dma: data");
    const o = out orelse return nullPtr("write_dma: out_dma_channel");
    if (!sd.argsOk(channel, len)) return common.k_ra8_err_invalid_arg;
    const req = sd.makeRequest(@intFromPtr(src), mmio(channel).base + off_tdr, len, true, false, done, ctx);
    return ra8_dma_request(&req, o);
}

export fn ra8_sci_read_dma(channel: u8, out_buf: ?[*]u8, len: u16, done: ?sd.DoneFn, ctx: ?*anyopaque, out: ?*u8) u16 {
    const dst = out_buf orelse return nullPtr("read_dma: out_buf");
    const o = out orelse return nullPtr("read_dma: out_dma_channel");
    if (!sd.argsOk(channel, len)) return common.k_ra8_err_invalid_arg;
    const req = sd.makeRequest(mmio(channel).base + off_rdr, @intFromPtr(dst), len, false, true, done, ctx);
    return ra8_dma_request(&req, o);
}

export fn ra8_sci_dispatch_txi(channel: u8) void {
    if (channel > sd.channel_max) return;
    sd.dispatchTxi(mmio(channel), &s_sci_state[channel]);
}

export fn ra8_sci_dispatch_rxi(channel: u8) void {
    if (channel > sd.channel_max) return;
    sd.dispatchRxi(mmio(channel), &s_sci_state[channel]);
}

export fn ra8_sci_dispatch_eri(channel: u8) void {
    if (channel > sd.channel_max) return;
    _ = ra8_sci_clear_errors(channel);
}
