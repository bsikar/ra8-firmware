//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! SSIE DMA attach/detach and isochronous FIFO send/receive (HUM Ch 46),
//! RA8FW-605. Pure logic; the C ABI lives in ssie_stream_abi.zig.
//! `hw` provides regs(ch) ?usize, read32(addr), write32(addr, u32),
//! runtime(ch) *Runtime, dmacStart(ch, *const DmacConfig) u16,
//! dmacStop(ch) u16, err(msg) and errVal(msg, u32).

pub const ok: u16 = 0;
pub const invalid_arg: u16 = 0x103;
pub const null_ptr: u16 = 0x504;

pub const channel_count: u8 = 2;
pub const dma_max_ch: u8 = 8;
pub const dma_ch_unused: u8 = 0xFF;
pub const fifo_depth: u32 = 32;
pub const off_ssifsr: usize = 0x14;
pub const off_ssiftdr: usize = 0x18;
pub const off_ssifrdr: usize = 0x1C;
const mask_rdc: u32 = 0x0000_3F00;
const mask_tdc: u32 = 0x3F00_0000;
const shift_rdc: u5 = 8;
const shift_tdc: u5 = 24;
pub const rdf_clear: u32 = 0x0001_0000;
pub const tde_clear: u32 = 0x0000_0001;
const dmac_width_word: u8 = 2;

/// `ra8_ssie_runtime_t`, owned by ra8_ssie.c as `g_ssie_runtime`.
pub const Runtime = extern struct {
    tx_dma_channel: u8 = dma_ch_unused,
    rx_dma_channel: u8 = dma_ch_unused,
    initialized: bool = false,
    dma_attached: bool = false,
};

/// `ra8_ssie_dma_cfg_t`.
pub const DmaCfg = extern struct {
    tx_dma_channel: u8 = dma_ch_unused,
    rx_dma_channel: u8 = dma_ch_unused,
    tx_buffer: ?[*]u32 = null,
    rx_buffer: ?[*]u32 = null,
    tx_samples: u16 = 0,
    rx_samples: u16 = 0,
};

/// `ra8_dmac_config_t`, 20 bytes; the fields this file leaves at zero
/// are zero exactly as the C designated initialisers leave them.
pub const DmacConfig = extern struct {
    src: u32 = 0,
    dst: u32 = 0,
    count: u16 = 0,
    width: u8 = 0,
    src_inc: bool = false,
    dst_inc: bool = false,
    mode: u8 = 0,
    block_count: u16 = 0,
    repeat_area: u8 = 0,
    irq_each: bool = false,
    enable_dtie: bool = false,
};

fn fail(hw: anytype, rc: u16, msg: [*:0]const u8) u16 {
    hw.err(msg);
    hw.errVal("Error", rc);
    return rc;
}

const Dirs = struct { tx: bool, rx: bool };

fn validate(dma: *const DmaCfg) ?Dirs {
    const d = Dirs{ .tx = dma.tx_dma_channel < dma_max_ch, .rx = dma.rx_dma_channel < dma_max_ch };
    if (!d.tx and !d.rx) return null;
    if (d.tx and (dma.tx_buffer == null or dma.tx_samples == 0)) return null;
    if (d.rx and (dma.rx_buffer == null or dma.rx_samples == 0)) return null;
    return d;
}

fn addr32(p: ?[*]u32) u32 {
    return @truncate(@intFromPtr(p));
}

fn startTx(hw: anytype, reg: usize, ch: u8, dma: *const DmaCfg) u16 {
    const cfg = DmacConfig{ .src = addr32(dma.tx_buffer), .dst = @truncate(reg + off_ssiftdr), .count = dma.tx_samples, .width = dmac_width_word, .src_inc = true };
    const rc = hw.dmacStart(dma.tx_dma_channel, &cfg);
    if (rc == ok) hw.runtime(ch).tx_dma_channel = dma.tx_dma_channel;
    return rc;
}

fn startRx(hw: anytype, reg: usize, ch: u8, dma: *const DmaCfg) u16 {
    const cfg = DmacConfig{ .src = @truncate(reg + off_ssifrdr), .dst = addr32(dma.rx_buffer), .count = dma.rx_samples, .width = dmac_width_word, .dst_inc = true };
    const rc = hw.dmacStart(dma.rx_dma_channel, &cfg);
    if (rc == ok) hw.runtime(ch).rx_dma_channel = dma.rx_dma_channel;
    return rc;
}

fn attachDirs(hw: anytype, reg: usize, ch: u8, dma: *const DmaCfg, d: Dirs) u16 {
    if (d.tx) {
        const rc = startTx(hw, reg, ch, dma);
        if (rc != ok) return fail(hw, rc, "ssie_attach_dma: tx start");
    }
    if (d.rx) {
        const rc = startRx(hw, reg, ch, dma);
        if (rc != ok) {
            if (d.tx) {
                _ = hw.dmacStop(dma.tx_dma_channel);
                hw.runtime(ch).tx_dma_channel = dma_ch_unused;
            }
            return rc;
        }
    }
    return ok;
}

/// `ra8_ssie_attach_dma`.
pub fn attachDma(hw: anytype, ch: u8, dma_opt: ?*const DmaCfg) u16 {
    const dma = dma_opt orelse {
        hw.err("dma must not be nullptr");
        return null_ptr;
    };
    const reg = hw.regs(ch) orelse return invalid_arg;
    const d = validate(dma) orelse return fail(hw, invalid_arg, "ssie_attach_dma: bad cfg");
    const rc = attachDirs(hw, reg, ch, dma, d);
    if (rc != ok) return fail(hw, rc, "ssie_attach_dma: dir start");
    hw.runtime(ch).dma_attached = true;
    return ok;
}

/// `ra8_ssie_detach_dma`.
pub fn detachDma(hw: anytype, ch: u8) u16 {
    if (ch >= channel_count) return invalid_arg;
    const rt = hw.runtime(ch);
    if (rt.tx_dma_channel < dma_max_ch) {
        _ = hw.dmacStop(rt.tx_dma_channel);
        rt.tx_dma_channel = dma_ch_unused;
    }
    if (rt.rx_dma_channel < dma_max_ch) {
        _ = hw.dmacStop(rt.rx_dma_channel);
        rt.rx_dma_channel = dma_ch_unused;
    }
    rt.dma_attached = false;
    return ok;
}

fn orUnused(dma_ch: u8) u8 {
    return if (dma_ch < dma_max_ch) dma_ch else dma_ch_unused;
}

/// `ra8_ssie_attach_dma_pair`: record the channels without starting them.
pub fn attachDmaPair(hw: anytype, ch: u8, tx: u8, rx: u8) u16 {
    if (ch >= channel_count) return invalid_arg;
    if (tx >= dma_max_ch and rx >= dma_max_ch) return invalid_arg;
    const rt = hw.runtime(ch);
    rt.tx_dma_channel = orUnused(tx);
    rt.rx_dma_channel = orUnused(rx);
    rt.dma_attached = true;
    return ok;
}

/// `ra8_ssie_send_iso`: push each sample once TDC shows room, then clear
/// TDE (W1C, RDF kept). Waits on TDC exactly as the C does.
pub fn sendIso(hw: anytype, ch: u8, buf_opt: ?[*]const u32, samples: u16) u16 {
    const buf = buf_opt orelse {
        hw.err("buffer must not be nullptr");
        return null_ptr;
    };
    const reg = hw.regs(ch) orelse return invalid_arg;
    var sent: u16 = 0;
    while (sent < samples) {
        const tdc = (hw.read32(reg + off_ssifsr) & mask_tdc) >> shift_tdc;
        if (tdc < fifo_depth) {
            hw.write32(reg + off_ssiftdr, buf[sent]);
            sent += 1;
        }
    }
    hw.write32(reg + off_ssifsr, rdf_clear);
    return ok;
}

/// `ra8_ssie_recv_iso`: drain up to `max` samples while RDC is non-zero.
pub fn recvIso(hw: anytype, ch: u8, buf_opt: ?[*]u32, max: u16, out_opt: ?*u16) u16 {
    const buf = buf_opt orelse {
        hw.err("buffer must not be nullptr");
        return null_ptr;
    };
    const out = out_opt orelse {
        hw.err("out_got must not be nullptr");
        return null_ptr;
    };
    const reg = hw.regs(ch) orelse return invalid_arg;
    var got: u16 = 0;
    while (got < max) : (got += 1) {
        if ((hw.read32(reg + off_ssifsr) & mask_rdc) >> shift_rdc == 0) break;
        buf[got] = hw.read32(reg + off_ssifrdr);
    }
    if (got > 0) hw.write32(reg + off_ssifsr, tde_clear);
    out.* = got;
    return ok;
}
