//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! VBATT tamper detection on RTCIC0..2 (HUM Ch 12.3.7.4), RA8FW-599.
//! Pure register sequences; the C ABI lives in bkup_tamper_abi.zig.
//! `regs` provides read(off) u8, write(off, u8) and prcr(u16); `c`
//! provides the log lines and the shared ra8_bkup.c state.

pub const base: usize = 0x4001_E000;
pub const off_prcr: usize = 0x3FA;
pub const off_vbtadsr: usize = 0xC48;
pub const off_vbtadcr1: usize = 0xC49;
pub const off_vbtadcr2: usize = 0xC4A;
pub const off_vbtictlr: usize = 0xC4C;
pub const off_vbtictlr2: usize = 0xC4D;
pub const off_vbtimonr: usize = 0xC4E;
pub const off_vbtncwcr: usize = 0xC50;
pub const off_vbtadcr3: usize = 0xC54;

/// PRCR key | PRC1 (low power + VBATT), and key alone to re-lock.
pub const prcr_unlock_lpm: u16 = 0xA502;
pub const prcr_lock_all: u16 = 0xA500;

pub const chan_count: u8 = 3;
pub const max_nc_width: u8 = 7;
const vincw_mask: u8 = 0x07;
const edge_rising: u8 = 1;
const capture_vbtadf: u8 = 1;

pub const ok: u16 = 0;
pub const invalid_arg: u16 = 0x103;
pub const null_ptr: u16 = 0x504;

/// `ra8_bkup_tamper_chan_cfg_t`, 7 bytes.
pub const ChanCfg = extern struct {
    input_enable: bool = false,
    noise_canceller_en: bool = false,
    edge: u8 = 0,
    irq_enable: bool = false,
    clear_backup: bool = false,
    zeroize_huk: bool = false,
    capture_src: u8 = 0,
};

/// `ra8_bkup_tamper_config_t`, 22 bytes.
pub const Config = extern struct {
    nc_width: u8 = 0,
    channels: [chan_count]ChanCfg = [_]ChanCfg{.{}} ** chan_count,
};

const range_msg = "tamper_init: channel cfg out of range";

fn chanMask(base_mask: u8, channel: u8) u8 {
    return base_mask << @intCast(channel);
}

fn chanValid(ch: ChanCfg) bool {
    return ch.edge <= edge_rising and ch.capture_src <= capture_vbtadf;
}

/// One bit per channel where `pick` holds, starting at `base_mask`.
fn compose(cfg: *const Config, base_mask: u8, comptime pick: fn (ChanCfg) bool) u8 {
    var value: u8 = 0;
    for (cfg.channels, 0..) |ch, i| {
        if (pick(ch)) value |= chanMask(base_mask, @intCast(i));
    }
    return value;
}

fn pickInput(ch: ChanCfg) bool {
    return ch.input_enable;
}
fn pickNce(ch: ChanCfg) bool {
    return ch.noise_canceller_en;
}
fn pickRising(ch: ChanCfg) bool {
    return ch.edge == edge_rising;
}
fn pickIrq(ch: ChanCfg) bool {
    return ch.irq_enable;
}
fn pickClear(ch: ChanCfg) bool {
    return ch.clear_backup;
}
fn pickCapture(ch: ChanCfg) bool {
    return ch.capture_src == capture_vbtadf;
}
fn pickZeroize(ch: ChanCfg) bool {
    return ch.zeroize_huk;
}

pub fn vbtictlr(cfg: *const Config) u8 {
    return compose(cfg, 0x01, pickInput);
}
pub fn vbtictlr2(cfg: *const Config) u8 {
    return compose(cfg, 0x01, pickNce) | compose(cfg, 0x10, pickRising);
}
pub fn vbtadcr1(cfg: *const Config) u8 {
    return compose(cfg, 0x01, pickIrq) | compose(cfg, 0x10, pickClear);
}
pub fn vbtadcr2(cfg: *const Config) u8 {
    return compose(cfg, 0x01, pickCapture);
}
pub fn vbtadcr3(cfg: *const Config) u8 {
    return compose(cfg, 0x01, pickZeroize);
}

/// `ra8_bkup_tamper_init`. A bad channel logs RA8_RETURN_ON_ERROR twice
/// (validator and caller), as the C did.
pub fn init(regs: anytype, c: anytype, cfg_opt: ?*const Config) u16 {
    const cfg = cfg_opt orelse {
        c.err("tamper cfg must not be nullptr");
        return null_ptr;
    };
    if (cfg.nc_width > max_nc_width) return invalid_arg;
    for (cfg.channels) |ch| {
        if (!chanValid(ch)) {
            c.fail(range_msg, invalid_arg);
            c.fail(range_msg, invalid_arg);
            return invalid_arg;
        }
    }
    regs.prcr(prcr_unlock_lpm);
    regs.write(off_vbtictlr2, 0);
    regs.write(off_vbtadcr1, 0);
    regs.write(off_vbtadcr2, 0);
    regs.write(off_vbtadcr3, 0);
    regs.write(off_vbtictlr, vbtictlr(cfg));
    regs.write(off_vbtncwcr, cfg.nc_width & vincw_mask);
    regs.write(off_vbtictlr2, vbtictlr2(cfg));
    _ = regs.read(off_vbtadsr);
    regs.write(off_vbtadsr, 0);
    regs.write(off_vbtadcr1, vbtadcr1(cfg));
    regs.write(off_vbtadcr2, vbtadcr2(cfg));
    regs.write(off_vbtadcr3, vbtadcr3(cfg));
    regs.prcr(prcr_lock_all);
    c.setInitialized();
    c.info("bkup_tamper_init");
    return ok;
}

/// `ra8_bkup_tamper_disable`.
pub fn disable(regs: anytype) u16 {
    regs.prcr(prcr_unlock_lpm);
    for ([_]usize{ off_vbtadcr1, off_vbtadcr2, off_vbtadcr3, off_vbtictlr2, off_vbtictlr, off_vbtadsr }) |off| {
        regs.write(off, 0);
    }
    regs.prcr(prcr_lock_all);
    return ok;
}

/// `ra8_bkup_read_input`.
pub fn readInput(regs: anytype, c: anytype, channel: u8, high_out: ?*bool) u16 {
    const out = high_out orelse {
        c.err("high_out must not be nullptr");
        return null_ptr;
    };
    if (channel >= chan_count) return invalid_arg;
    out.* = (regs.read(off_vbtimonr) & chanMask(0x01, channel)) != 0;
    return ok;
}

/// `ra8_bkup_set_input_enable`; the protected read-modify-write is
/// ra8_bkup.c's priv_ra8_bkup_internal_rmw8, reached through `c`.
pub fn setInputEnable(c: anytype, channel: u8, enable: bool) u16 {
    if (channel >= chan_count) return invalid_arg;
    c.rmw(off_vbtictlr, chanMask(0x01, channel), enable, prcr_unlock_lpm);
    return ok;
}
