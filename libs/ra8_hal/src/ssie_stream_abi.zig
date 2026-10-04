//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the SSIE DMA and isochronous FIFO functions in ra8_ssie.h
//! (RA8FW-605). ra8_ssie.c still owns g_ssie_runtime and the channel
//! register lookup; they are reached here as C symbols.

const common = @import("abi_common.zig");
const s = @import("internal/ssie_stream.zig");

const tag = "SSIE";

extern var g_ssie_runtime: [s.channel_count]s.Runtime;
extern fn priv_ra8_ssie_internal_regs(channel: u8) ?*volatile anyopaque;
extern fn ra8_dmac_start(channel: u8, cfg: *const s.DmacConfig) u16;
extern fn ra8_dmac_stop(channel: u8) u16;

const Hw = struct {
    pub fn regs(_: Hw, ch: u8) ?usize {
        const p = priv_ra8_ssie_internal_regs(ch) orelse return null;
        return @intFromPtr(p);
    }
    pub fn read32(_: Hw, addr: usize) u32 {
        return @as(*volatile u32, @ptrFromInt(addr)).*;
    }
    pub fn write32(_: Hw, addr: usize, value: u32) void {
        @as(*volatile u32, @ptrFromInt(addr)).* = value;
    }
    pub fn runtime(_: Hw, ch: u8) *s.Runtime {
        return &g_ssie_runtime[ch];
    }
    pub fn dmacStart(_: Hw, ch: u8, cfg: *const s.DmacConfig) u16 {
        return ra8_dmac_start(ch, cfg);
    }
    pub fn dmacStop(_: Hw, ch: u8) u16 {
        return ra8_dmac_stop(ch);
    }
    pub fn err(_: Hw, msg: [*:0]const u8) void {
        common.ra8_log_emit_error(tag, msg);
    }
    pub fn errVal(_: Hw, msg: [*:0]const u8, value: u32) void {
        common.ra8_log_emit_error_val(tag, msg, value);
    }
};

export fn ra8_ssie_attach_dma(channel: u8, dma: ?*const s.DmaCfg) u16 {
    return s.attachDma(Hw{}, channel, dma);
}

export fn ra8_ssie_detach_dma(channel: u8) u16 {
    return s.detachDma(Hw{}, channel);
}

export fn ra8_ssie_attach_dma_pair(channel: u8, tx_dma_channel: u8, rx_dma_channel: u8) u16 {
    return s.attachDmaPair(Hw{}, channel, tx_dma_channel, rx_dma_channel);
}

export fn ra8_ssie_send_iso(channel: u8, buffer: ?[*]const u32, samples: u16) u16 {
    return s.sendIso(Hw{}, channel, buffer, samples);
}

export fn ra8_ssie_recv_iso(channel: u8, buffer: ?[*]u32, max_samples: u16, out_got: ?*u16) u16 {
    return s.recvIso(Hw{}, channel, buffer, max_samples, out_got);
}
