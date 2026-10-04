//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! PDM-IF digital microphone driver (RA8FW-595). Pure: `regs` reads and
//! writes 32-bit registers at an offset from the PDM base, `c` reaches the
//! module stop, ISR and log services. Offsets follow ra8_pdm_regs.h.

pub const ok: u16 = 0;
pub const err_invalid_arg: u16 = 0x103;
pub const err_exists: u16 = 0x10C;
pub const err_not_initialized: u16 = 0x10F;
pub const err_hw_timeout: u16 = 0x203;
pub const err_null_ptr: u16 = 0x504;

pub const base: usize = 0x40256000;
pub const ch_count: u8 = 3;
pub const fifo_depth: u32 = 32;
pub const mstp_pdmif: u16 = (2 << 8) | 24; // MSTPC24
pub const events = [ch_count]u16{ 0x0BF, 0x0C0, 0x0C1 }; // PDM_DAT0..2

const prime_discards: u32 = 30; // FIFO depth (32) minus 2 priming reads
const stop_poll_max: u32 = 1000;

pub const pdcstrtr: usize = 0x00;
pub const pdcstptr: usize = 0x04;
pub const pdcicr: usize = 0x0C;
pub const pdcsr: usize = 0x10;
pub const pdcdrcr: usize = 0x24;

pub const pdicr: usize = 0x0C;
pub const pdscr: usize = 0x18;
pub const pdmdsr: usize = 0x20;
pub const pdsfcr: usize = 0x24;
pub const pdhfcs0r: usize = 0x28;
pub const pdhfck1r: usize = 0x2C;
pub const pdhfchr: usize = 0x30;
pub const pdcfchr: usize = 0x38;
pub const pdlfch010r: usize = 0x64;
pub const pdlfch1r: usize = 0x68;
pub const pddbcr: usize = 0xC0;
pub const pddrcr: usize = 0xE0;
pub const pddrr: usize = 0xE8;
pub const pddsr: usize = 0xEC;

const pdscr_clr_all: u32 = 0x08070002;
const pdicr_idre: u32 = 0x4;

/// Offset of a channel register from the PDM base.
pub fn chOff(ch: u8, off: usize) usize {
    return 0x100 + @as(usize, ch) * 0x100 + off;
}

/// `ra8_pdm_channel_cfg_t`.
pub const Config = extern struct {
    sinc_order: u8,
    clock_div: u8,
    sinc_dec: u8,
    sinc_range: u8,
    data_shift: u8,
    edge: u8,
    hpf_shift: u8,
    cf_shift: u8,
    lpf_shift: u8,
    rx_threshold: u8,
    hpf_s0: u16,
    hpf_k1: u16,
    hpf_h: [2]u16,
    comp_h: [11]u16,
    lpf_h0: u16,
    lpf_h1: [20]u16,
};

/// `ra8_pdm_data_callback_t`.
pub const DataFn = *const fn (ctx: ?*anyopaque, samples: [*]const i32, count: u32) callconv(.C) void;

pub const Stream = struct { callback: ?DataFn = null, ctx: ?*anyopaque = null };

fn range(c: anytype, ch: u8) ?u16 {
    if (ch <= ch_count - 1) return null;
    c.err("Range check failed");
    return err_invalid_arg;
}

pub fn modeWord(cfg: *const Config) u32 {
    var v: u32 = cfg.edge & 1;
    v |= (@as(u32, cfg.sinc_order) & 7) << 4;
    v |= @as(u32, cfg.hpf_shift) << 8;
    v |= @as(u32, cfg.cf_shift) << 12;
    v |= @as(u32, cfg.lpf_shift) << 16;
    v |= @as(u32, cfg.data_shift) << 28;
    return v;
}

pub fn filterWord(cfg: *const Config) u32 {
    return @as(u32, cfg.clock_div) | @as(u32, cfg.sinc_dec) << 16 | @as(u32, cfg.sinc_range) << 24;
}

fn writeCoeffs(regs: anytype, ch: u8, cfg: *const Config) void {
    regs.write32(chOff(ch, pdhfcs0r), cfg.hpf_s0);
    regs.write32(chOff(ch, pdhfck1r), cfg.hpf_k1);
    for (cfg.hpf_h, 0..) |h, i| regs.write32(chOff(ch, pdhfchr + 4 * i), h);
    for (cfg.comp_h, 0..) |h, i| regs.write32(chOff(ch, pdcfchr + 4 * i), h);
    regs.write32(chOff(ch, pdlfch010r), cfg.lpf_h0);
    for (cfg.lpf_h1, 0..) |h, i| regs.write32(chOff(ch, pdlfch1r + 4 * i), h);
}

/// PDDRR[19:0] is signed 20-bit PCM.
pub fn signExtend20(raw: u32) i32 {
    const dat: u20 = @truncate(raw);
    return @as(i20, @bitCast(dat));
}

pub fn init(regs: anytype, c: anytype) u16 {
    const err = c.mstpEnable(mstp_pdmif);
    if (err != ok) {
        c.fail("pdm_init: mstp enable", err);
        return err;
    }
    regs.write32(pdcicr, 0);
    regs.write32(pdcdrcr, 0);
    c.info("pdm_init");
    return ok;
}

pub fn deinit(regs: anytype, c: anytype) u16 {
    regs.write32(pdcdrcr, 0);
    return c.mstpDisable(mstp_pdmif);
}

pub fn configure(regs: anytype, c: anytype, ch: u8, cfg: ?*const Config) u16 {
    const k = cfg orelse {
        c.err("cfg must not be nullptr");
        return err_null_ptr;
    };
    if (range(c, ch)) |e| return e;
    regs.write32(chOff(ch, pdmdsr), modeWord(k));
    regs.write32(chOff(ch, pdsfcr), filterWord(k));
    writeCoeffs(regs, ch, k);
    regs.write32(chOff(ch, pddbcr), k.rx_threshold);
    return ok;
}

pub fn start(regs: anytype, c: anytype, ch: u8) u16 {
    if (range(c, ch)) |e| return e;
    regs.write32(pdcstrtr, @as(u32, 1) << @intCast(ch));
    return ok;
}

pub fn readEnable(regs: anytype, c: anytype, ch: u8) u16 {
    if (range(c, ch)) |e| return e;
    regs.write32(chOff(ch, pdscr), pdscr_clr_all);
    regs.write32(chOff(ch, pddrcr), 1);
    for (0..prime_discards) |_| _ = regs.read32(chOff(ch, pddrr));
    return ok;
}

fn drain(regs: anytype, ch: u8, out: [*]i32, n: u32) void {
    for (0..n) |i| out[i] = signExtend20(regs.read32(chOff(ch, pddrr)));
}

pub fn read(regs: anytype, c: anytype, ch: u8, out: ?[*]i32, max: u32, out_count: ?*u32) u16 {
    const dst = out orelse {
        c.err("out must not be nullptr");
        return err_null_ptr;
    };
    const cnt = out_count orelse {
        c.err("out_count must not be nullptr");
        return err_null_ptr;
    };
    if (range(c, ch)) |e| return e;
    if (max == 0) {
        c.err("read: max must be > 0");
        return err_invalid_arg;
    }
    const n = @min(regs.read32(chOff(ch, pddsr)) & 0xFF, max);
    drain(regs, ch, dst, n);
    cnt.* = n;
    return ok;
}

pub fn streamEnable(regs: anytype, c: anytype, streams: *[ch_count]Stream, ch: u8, cb: ?DataFn, ctx: ?*anyopaque, priority: u8) u16 {
    const f = cb orelse {
        c.err("callback must not be nullptr");
        return err_null_ptr;
    };
    if (range(c, ch)) |e| return e;
    if (streams[ch].callback != null) return err_exists;
    streams[ch] = .{ .callback = f, .ctx = ctx };
    const err = c.isrRegister(events[ch], ch, priority);
    if (err != ok) {
        streams[ch] = .{};
        return err;
    }
    const off = chOff(ch, pdicr);
    regs.write32(off, regs.read32(off) | pdicr_idre);
    return ok;
}

pub fn streamDisable(regs: anytype, c: anytype, streams: *[ch_count]Stream, ch: u8) u16 {
    if (range(c, ch)) |e| return e;
    if (streams[ch].callback == null) return err_not_initialized;
    const off = chOff(ch, pdicr);
    regs.write32(off, regs.read32(off) & ~pdicr_idre);
    const err = c.isrUnregister(events[ch]);
    if (err != ok) return err;
    streams[ch] = .{};
    return ok;
}

pub fn stop(regs: anytype, c: anytype, streams: *[ch_count]Stream, ch: u8) u16 {
    if (range(c, ch)) |e| return e;
    if (streams[ch].callback != null) {
        const err = streamDisable(regs, c, streams, ch);
        if (err != ok) {
            c.fail("pdm_stop: stream disable", err);
            return err;
        }
    }
    regs.write32(chOff(ch, pddrcr), 0);
    const bit = @as(u32, 1) << @intCast(ch);
    regs.write32(pdcstptr, bit);
    for (0..stop_poll_max) |_| {
        if (regs.read32(pdcsr) & bit == 0) return ok;
    }
    c.err("stop: channel did not halt");
    return err_hw_timeout;
}

/// Data-reception ISR body: drain up to one FIFO into the stream callback.
pub fn dataIsr(regs: anytype, streams: *const [ch_count]Stream, ch: u8) void {
    if (ch >= ch_count) return;
    const s = streams[ch];
    const cb = s.callback orelse return;
    const count = @min(regs.read32(chOff(ch, pddsr)) & 0xFF, fifo_depth);
    var samples: [fifo_depth]i32 = undefined;
    drain(regs, ch, &samples, count);
    if (count != 0) cb(s.ctx, &samples, count);
}
