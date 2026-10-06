//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! DMAC channel driver logic (RA8FW-746), ported from the C driver. Register
//! access, MSTP and logging go through an `ops` value so host tests can run
//! the driver against in-memory register blocks. HUM chapter 17.

const dma = @import("dma.zig");

pub const Config = dma.Config;
pub const CallbackFn = *const fn (ctx: ?*anyopaque) callconv(.c) void;

pub const ok: u16 = 0;
pub const invalid_arg: u16 = 0x103;
pub const hw_timeout: u16 = 0x203;
pub const out_of_range: u16 = 0x208;
pub const null_ptr: u16 = 0x504;

pub const channel_count: u8 = 8;
pub const mstp_dmac0_dtc0: u16 = dma.mstp_dmac0_dtc0;

/// Public enum values from ra8_dmac.h.
pub const width_word: u8 = 2;
pub const mode_normal: u8 = 0;
pub const mode_repeat: u8 = 1;
pub const mode_block: u8 = 2;
pub const mode_repeat_block: u8 = 3;
pub const area_dest: u8 = 0;
pub const area_src: u8 = 1;
pub const addr_decrement: u8 = 3;

/// Mirror of `r_dmac_channel_regs_t` (ra8_dmac_regs.h), one 0x40 stride.
pub const ChannelRegs = extern struct {
    dmsar: u32 = 0,
    dmdar: u32 = 0,
    dmcra: u32 = 0,
    dmcrb: u32 = 0,
    dmtmd: u16 = 0,
    _r0: u8 = 0,
    dmint: u8 = 0,
    dmamd: u16 = 0,
    _r1: u16 = 0,
    dmofr: u32 = 0,
    dmcnt: u8 = 0,
    dmreq: u8 = 0,
    dmsts: u8 = 0,
    _r2: u8 = 0,
    dmsrr: u32 = 0,
    dmdrr: u32 = 0,
    dmsbs: u32 = 0,
    dmdbs: u32 = 0,
    dmbwr: u8 = 0,
    _r3: u8 = 0,
    _r4: u16 = 0,
    _r5: [12]u8 = @splat(0),
};

comptime {
    if (@sizeOf(ChannelRegs) != 0x40) @compileError("ChannelRegs size");
    if (@offsetOf(ChannelRegs, "dmtmd") != 0x10) @compileError("DMTMD offset");
    if (@offsetOf(ChannelRegs, "dmint") != 0x13) @compileError("DMINT offset");
    if (@offsetOf(ChannelRegs, "dmamd") != 0x14) @compileError("DMAMD offset");
    if (@offsetOf(ChannelRegs, "dmofr") != 0x18) @compileError("DMOFR offset");
    if (@offsetOf(ChannelRegs, "dmcnt") != 0x1C) @compileError("DMCNT offset");
    if (@offsetOf(ChannelRegs, "dmsts") != 0x1E) @compileError("DMSTS offset");
    if (@offsetOf(ChannelRegs, "dmbwr") != 0x30) @compileError("DMBWR offset");
}

// Register field encodings (ra8_dmac_regs.h).
const dmtmd_sz_pos = 8;
const dmtmd_dts_pos = 12;
const dmtmd_md_pos = 14;
const dts_none: u16 = 2;
const dmamd_dm_pos = 6;
const dmamd_sm_pos = 14;
const dmamd_dm_mask: u16 = 0x3 << dmamd_dm_pos;
const dmamd_sm_mask: u16 = 0x3 << dmamd_sm_pos;
const dmamd_increment: u16 = 2;
pub const dmint_rptie: u8 = 1 << 2;
pub const dmint_esie: u8 = 1 << 3;
pub const dmint_dtie: u8 = 1 << 4;
const dmcra_high_pos = 16;
const dmcra_mask: u32 = (0x3FF << dmcra_high_pos) | 0x3FF;
pub const dmcnt_dte: u8 = 1;
pub const dmreq_swreq: u8 = 1;
pub const dmsts_act: u8 = 1 << 7;
pub const dmast_dmst: u8 = 1;

/// `priv_ra8_dmac_internal_mode_disables_dts`.
pub fn modeDisablesDts(normal_val: u32, repeat_block_val: u32, mode: u32) bool {
    return mode == normal_val or mode == repeat_block_val;
}

/// `priv_ra8_dmac_internal_dmint_extra_irq`.
pub fn dmintExtraIrq(irq_each: bool, repeat_block_val: u32, mode: u32) bool {
    return irq_each and mode != repeat_block_val;
}

/// DMTMD.SZ code; out-of-range widths fall back to byte like the C.
fn szCode(width: u8) u16 {
    return if (width <= width_word) width else 0;
}

/// DMTMD.MD code; the public enum matches the field encoding.
fn mdCode(mode: u8) u16 {
    return if (mode <= mode_repeat_block) mode else 0;
}

/// DMTMD.DTS code: none for normal and repeat-block, else the area.
fn dtsCode(mode: u8, area: u8) u16 {
    if (modeDisablesDts(mode_normal, mode_repeat_block, mode)) return dts_none;
    return switch (area) {
        area_dest => 0,
        area_src => 1,
        else => dts_none,
    };
}

/// DMAMD value with SM/DM set to increment as requested (HUM 17.2.12).
pub fn dmamdValue(src_inc: bool, dst_inc: bool) u16 {
    var v: u16 = 0;
    if (src_inc) v |= dmamd_increment << dmamd_sm_pos;
    if (dst_inc) v |= dmamd_increment << dmamd_dm_pos;
    return v;
}

/// DMTMD value for `cfg` (HUM 17.2.10).
pub fn dmtmdValue(cfg: *const Config) u16 {
    return (szCode(cfg.width) << dmtmd_sz_pos) |
        (mdCode(cfg.mode) << dmtmd_md_pos) |
        (dtsCode(cfg.mode, cfg.repeat_area) << dmtmd_dts_pos);
}

/// DMINT value for `cfg` (HUM 17.2.11).
pub fn dmintValue(cfg: *const Config) u8 {
    var v: u8 = 0;
    if (cfg.enable_dtie) v |= dmint_dtie;
    if (dmintExtraIrq(cfg.irq_each, mode_repeat_block, cfg.mode)) v |= dmint_rptie | dmint_esie;
    return v;
}

/// DMCRA value: count in both halves for the non-normal modes (HUM 17.2.8).
pub fn dmcraValue(cfg: *const Config) u32 {
    var v: u32 = cfg.count;
    if (cfg.mode != mode_normal) {
        v |= @as(u32, cfg.count) << dmcra_high_pos;
        v &= dmcra_mask;
    }
    return v;
}

fn validateCfg(cfg: *const Config) u16 {
    if (cfg.width > width_word) return invalid_arg;
    if (cfg.mode > mode_repeat_block) return invalid_arg;
    return ok;
}

/// Program every channel register in the HUM order, DTE cleared first.
fn programChannel(reg: *volatile ChannelRegs, cfg: *const Config) void {
    reg.dmcnt = 0;
    reg.dmtmd = dmtmdValue(cfg);
    reg.dmamd = dmamdValue(cfg.src_inc, cfg.dst_inc);
    reg.dmsar = cfg.src;
    reg.dmdar = cfg.dst;
    reg.dmcra = dmcraValue(cfg);
    if (cfg.mode == mode_normal) {
        reg.dmcrb = 0;
    } else {
        const bc: u32 = cfg.block_count;
        reg.dmcrb = bc | (bc << dmcra_high_pos);
    }
    reg.dmofr = 0;
    reg.dmint = dmintValue(cfg);
}

/// `ra8_dmac_start`.
pub fn start(ops: anytype, channel: u8, cfg_opt: ?*const Config) u16 {
    const cfg = cfg_opt orelse return ops.nullPtr("cfg must not be nullptr");
    const verr = validateCfg(cfg);
    if (verr != ok) return verr;
    const reg = ops.channel(channel) orelse return out_of_range;
    const mst_err = ops.mstpEnable(mstp_dmac0_dtc0);
    if (mst_err != ok) {
        ops.fail("dmac_start: mstp enable", mst_err);
        return mst_err;
    }
    programChannel(reg, cfg);
    ops.dmast().* = dmast_dmst;
    reg.dmcnt = dmcnt_dte;
    ops.infoVal("dmac_start ch", channel);
    return ok;
}

/// `ra8_dmac_stop`: clear DTE and drop the MSTP reference from start.
pub fn stop(ops: anytype, channel: u8) u16 {
    const reg = ops.channel(channel) orelse return out_of_range;
    reg.dmcnt = 0;
    return ops.mstpDisable(mstp_dmac0_dtc0);
}

/// Start a copy of `cfg` with `mode` forced; the caller's cfg is untouched.
pub fn startWithMode(ops: anytype, channel: u8, cfg_opt: ?*const Config, mode: u8) u16 {
    const cfg = cfg_opt orelse return ops.nullPtr("cfg must not be nullptr");
    var local = cfg.*;
    local.mode = mode;
    return start(ops, channel, &local);
}

/// `ra8_dmac_start_block`: block_count must be non-zero.
pub fn startBlock(ops: anytype, channel: u8, cfg_opt: ?*const Config) u16 {
    const cfg = cfg_opt orelse return ops.nullPtr("cfg must not be nullptr");
    if (cfg.block_count == 0) return invalid_arg;
    return startWithMode(ops, channel, cfg, mode_block);
}

/// `ra8_dmac_set_address_mode`: rewrite SM/DM, keep the other DMAMD bits.
pub fn setAddressMode(ops: anytype, channel: u8, src_mode: u8, dest_mode: u8) u16 {
    if (src_mode > addr_decrement or dest_mode > addr_decrement) return invalid_arg;
    const reg = ops.channel(channel) orelse return out_of_range;
    var v: u16 = reg.dmamd;
    v &= ~(dmamd_sm_mask | dmamd_dm_mask);
    v |= @as(u16, src_mode) << dmamd_sm_pos;
    v |= @as(u16, dest_mode) << dmamd_dm_pos;
    reg.dmamd = v;
    return ok;
}

/// `ra8_dmac_software_trigger` (HUM 17.2.15 DMREQ).
pub fn softwareTrigger(ops: anytype, channel: u8) u16 {
    const reg = ops.channel(channel) orelse return out_of_range;
    reg.dmreq = dmreq_swreq;
    return ok;
}

/// `ra8_dmac_is_active` (HUM 17.2.16 DMSTS.ACT).
pub fn isActive(ops: anytype, channel: u8, out: ?*bool) u16 {
    const out_active = out orelse return ops.nullPtr("out_active must not be nullptr");
    const reg = ops.channel(channel) orelse return out_of_range;
    out_active.* = (reg.dmsts & dmsts_act) != 0;
    return ok;
}

/// `ra8_dmac_wait_idle`: poll DMSTS.ACT up to `poll_limit` times.
pub fn waitIdle(ops: anytype, channel: u8, poll_limit: u32) u16 {
    const reg = ops.channel(channel) orelse return out_of_range;
    var i: u32 = 0;
    while (i < poll_limit) : (i += 1) {
        if ((reg.dmsts & dmsts_act) == 0) return ok;
    }
    return hw_timeout;
}

const Slot = struct {
    full_fn: ?CallbackFn = null,
    full_ctx: ?*anyopaque = null,
    half_fn: ?CallbackFn = null,
    half_ctx: ?*anyopaque = null,
};

/// Per-channel DTIE (full) and RPTIE (half) user handlers.
pub const Slots = struct {
    slots: [channel_count]Slot = @splat(.{}),

    pub fn attach(self: *Slots, channel: u8, half: bool, f: ?CallbackFn, ctx: ?*anyopaque) u16 {
        if (channel >= channel_count) return out_of_range;
        const s = &self.slots[channel];
        if (half) {
            s.half_fn = f;
            s.half_ctx = ctx;
        } else {
            s.full_fn = f;
            s.full_ctx = ctx;
        }
        return ok;
    }

    pub fn dispatch(self: *const Slots, channel: u8, half: bool) void {
        if (channel >= channel_count) return;
        const s = self.slots[channel];
        const f = if (half) s.half_fn else s.full_fn;
        const ctx = if (half) s.half_ctx else s.full_ctx;
        if (f) |cb| cb(ctx);
    }
};
