//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! DTC driver (RA8FW-588, was ra8_dtc.c). Pure: the DTC0 register block
//! comes in through a `regs` value (read/write by offset and width) and
//! MSTP, cache clean, ICU DTCE routing and logging through an `ops` value.
//! HUM Ch 18 "Data Transfer Controller" p 786..799.

pub const ok: u16 = 0;
pub const invalid_arg: u16 = 0x103;
pub const invalid_state: u16 = 0x104;
pub const null_ptr: u16 = 0x504;

pub const base_addr: usize = 0x4000_AC00;
pub const off_cr: usize = 0x00; // DTCCR, u8
pub const off_vbr: usize = 0x04; // DTCVBR, u32 (Non-secure alias)
pub const off_st: usize = 0x0C; // DTCST, u8
pub const off_sts: usize = 0x0E; // DTCSTS, u16
pub const off_vbr_sec: usize = 0x14; // DTCVBR_SEC, u32

pub const cr_rrs_disable: u8 = 0x08;
pub const cr_rrs_enable: u8 = 0x18;
pub const st_start: u8 = 0x01;
/// MSTPA22, shared by DMAC0 and DTC0 (ra8_mstp keeps the ref count).
pub const mstp_dmac0_dtc0: u16 = 22;

pub const ti_size: u32 = 16;
pub const ti_align: usize = 16;
pub const table_align: u32 = 1024;
pub const vector_entries: u16 = 96;
pub const block_units_max: u16 = 256;

pub const mode_normal: u8 = 0;
pub const mode_block: u8 = 2;
pub const unit_byte: u8 = 0;
pub const unit_half: u8 = 1;
pub const unit_word: u8 = 2;
pub const addr_fixed: u8 = 0;
pub const addr_inc: u8 = 2;

/// `r_dtc_xfer_info_t`: one 16-byte transfer-information block.
pub const Ti = extern struct { mr: u32, sar: u32, dar: u32, crb: u16, cra: u16 };

/// `ra8_dtc_xfer_cfg_t`.
pub const Cfg = extern struct {
    src: ?*const anyopaque,
    dst: ?*anyopaque,
    src_mode: u8,
    dst_mode: u8,
    unit: u8,
    mode: u8,
    unit_count: u16,
    block_count: u16,
};

pub const EventFn = *const fn (ctx: ?*anyopaque, status: u16) callconv(.c) void;

fn modeValid(m: u8) bool {
    return m == mode_normal or m == mode_block;
}

fn unitValid(u: u8) bool {
    return u == unit_byte or u == unit_half or u == unit_word;
}

fn addrValid(a: u8) bool {
    return a == addr_fixed or a == addr_inc;
}

fn addr32(p: anytype) u32 {
    return @truncate(@intFromPtr(p));
}

/// CRA/CRB for the transfer mode, or null when the counts don't fit it.
/// Block mode: CRAH = CRAL = block size, 256 encoded as 0 (HUM p 790).
fn counts(cfg: *const Cfg) ?[2]u16 {
    if (cfg.mode == mode_block) {
        if (cfg.unit_count == 0 or cfg.unit_count > block_units_max or cfg.block_count == 0) return null;
        const size8: u16 = cfg.unit_count & 0xFF;
        return .{ (size8 << 8) | size8, cfg.block_count };
    }
    if (cfg.unit_count == 0 or cfg.block_count != 0) return null;
    return .{ cfg.unit_count, 0 };
}

/// Fills a TI block from `cfg`; MRC stays 0 (no chained transfer).
pub fn describe(ops: anytype, cfg_in: ?*const Cfg, ti_in: ?*Ti) u16 {
    const cfg = cfg_in orelse return ops.nullPtr("cfg must not be nullptr");
    const ti = ti_in orelse return ops.nullPtr("out_ti must not be nullptr");
    const src = cfg.src orelse return ops.nullPtr("cfg->src must not be nullptr");
    const dst = cfg.dst orelse return ops.nullPtr("cfg->dst must not be nullptr");
    if (!modeValid(cfg.mode) or !unitValid(cfg.unit) or !addrValid(cfg.src_mode) or !addrValid(cfg.dst_mode)) {
        ops.logError("describe: unsupported mode/unit/address encoding");
        return invalid_arg;
    }
    const cr = counts(cfg) orelse {
        if (cfg.mode == mode_block) {
            ops.logError("describe: block counts out of range");
        } else {
            ops.logError("describe: normal mode wants a unit count and no block count");
        }
        return invalid_arg;
    };
    const mra: u32 = (@as(u32, cfg.mode & 3) << 6) | (@as(u32, cfg.unit & 3) << 4) | (@as(u32, cfg.src_mode & 3) << 2);
    const mrb: u32 = @as(u32, cfg.dst_mode & 3) << 2;
    ti.mr = (mra << 24) | (mrb << 16);
    ti.sar = addr32(src);
    ti.dar = addr32(dst);
    ti.crb = cr[1];
    ti.cra = cr[0];
    return ok;
}

pub const State = struct {
    handler: ?EventFn = null,
    ctx: ?*anyopaque = null,
    vector_base: ?*anyopaque = null,

    /// Both vector bases are written: a secure engine reads DTCVBR_SEC and
    /// drops secure writes to the Non-secure DTCVBR alias (HUM p 789).
    pub fn init(s: *State, regs: anytype, ops: anytype, base_in: ?*anyopaque) u16 {
        const base = base_in orelse return ops.nullPtr("vector_base must not be nullptr");
        const e = ops.mstpEnable(mstp_dmac0_dtc0);
        if (e != ok) {
            ops.fail("dtc_init: mstp enable", e);
            return e;
        }
        regs.write8(off_cr, 0);
        regs.write32(off_vbr, addr32(base));
        regs.write32(off_vbr_sec, addr32(base));
        regs.write8(off_st, 0);
        s.vector_base = base;
        ops.logInfo("dtc_init");
        return ok;
    }

    pub fn deinit(s: *State, regs: anytype, ops: anytype) u16 {
        regs.write8(off_st, 0);
        regs.write8(off_cr, 0);
        regs.write32(off_vbr, 0);
        regs.write32(off_vbr_sec, 0);
        s.* = .{};
        return ops.mstpDisable(mstp_dmac0_dtc0);
    }

    /// Cleans the vector table so the engine sees entries written since init.
    pub fn enable(s: *const State, regs: anytype, ops: anytype) u16 {
        if (s.vector_base) |b| _ = ops.cacheClean(b, table_align);
        regs.write8(off_st, st_start);
        return ok;
    }

    /// New table, then an RRS toggle to drop the engine's cached TI reads.
    pub fn reconfigure(s: *State, regs: anytype, ops: anytype, base_in: ?*anyopaque) u16 {
        const base = base_in orelse return ops.nullPtr("vector_base must not be nullptr");
        regs.write8(off_st, 0);
        regs.write32(off_vbr, addr32(base));
        regs.write32(off_vbr_sec, addr32(base));
        s.vector_base = base;
        _ = ops.cacheClean(base, table_align);
        regs.write8(off_cr, cr_rrs_disable);
        regs.write8(off_cr, cr_rrs_enable);
        return ok;
    }

    pub fn attach(s: *State, f: ?EventFn, ctx: ?*anyopaque) u16 {
        s.handler = f;
        s.ctx = ctx;
        return ok;
    }

    /// ISR path: latch DTCSTS, clear it, then call the handler.
    pub fn dispatch(s: *const State, regs: anytype) void {
        const mask = regs.read16(off_sts);
        const f = s.handler;
        const ctx = s.ctx;
        regs.write16(off_sts, 0);
        if (f) |cb| cb(ctx, mask);
    }

    /// Describe, point vector slot `slot` at the TI, clean both, set DTCE.
    pub fn bind(s: *const State, ops: anytype, slot: u16, cfg_in: ?*const Cfg, ti_in: ?*Ti) u16 {
        const cfg = cfg_in orelse return ops.nullPtr("cfg must not be nullptr");
        const ti = ti_in orelse return ops.nullPtr("ti must not be nullptr");
        const base = s.vector_base orelse {
            ops.logError("bind_activation: ra8_dtc_init has not run");
            return invalid_state;
        };
        if (slot >= vector_entries) {
            ops.logError("bind_activation: slot outside the vector table");
            return invalid_arg;
        }
        const d = describe(ops, cfg, ti);
        if (d != ok) return d;
        const table: [*]u32 = @ptrCast(@alignCast(base));
        table[slot] = addr32(ti);
        _ = ops.cacheClean(ti, ti_size);
        _ = ops.cacheClean(base, table_align);
        return ops.isrSetDtc(slot, true);
    }
};

pub fn disable(regs: anytype) u16 {
    regs.write8(off_st, 0);
    return ok;
}

pub fn status(regs: anytype) u16 {
    return regs.read16(off_sts);
}

pub fn clearStatus(regs: anytype, mask: u16) u16 {
    regs.write16(off_sts, regs.read16(off_sts) & ~mask);
    return ok;
}

pub fn enterStop(regs: anytype, ops: anytype) u16 {
    regs.write8(off_st, 0);
    return ops.mstpDisable(mstp_dmac0_dtc0);
}

pub fn exitStop(ops: anytype) u16 {
    return ops.mstpEnable(mstp_dmac0_dtc0);
}
