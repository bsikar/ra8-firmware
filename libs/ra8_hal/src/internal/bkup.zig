//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! VBATT battery backup domain (HUM Ch 12), RA8FW-601. Pure register
//! sequences; the C ABI and the domain's shared state live in bkup_abi.zig.
//! `regs` provides read8/write8(off), read32/write32(off) and prcr(u16);
//! `c` provides the log lines, the VBAE settle spin and the shared state.

pub const base: usize = 0x4001_E000;
pub const off_prcr: usize = 0x3FA;
pub const off_vbattmnselr: usize = 0xA84;
pub const off_vbtbpcr1: usize = 0xA88;
pub const off_vbtber: usize = 0xC40;
pub const off_vbtbpcr2: usize = 0xC45;
pub const off_vbtbpsr: usize = 0xC46;
pub const off_vbtadsr: usize = 0xC48;
pub const off_vbtadcr1: usize = 0xC49;
pub const off_vbtadcr2: usize = 0xC4A;
pub const off_vbtictlr: usize = 0xC4C;
pub const off_vbtictlr2: usize = 0xC4D;
pub const off_vbtbkr0: usize = 0xD00;

pub const reg_count: u16 = 128;
pub const word_count: u8 = 32;

/// PRCR key | PRC1 (low power + VBATT), key | PRC3 (PVD + VBATTMNSELR).
pub const unlock_lpm: u16 = 0xA502;
pub const unlock_pvd: u16 = 0xA508;
pub const lock_all: u16 = 0xA500;

const vbae: u8 = 0x08;
const bpwswstp: u8 = 0x01;
const vdete: u8 = 0x10;
const lvl_mask: u8 = 0x07;
pub const vbporf: u8 = 0x01;
pub const vbporm: u8 = 0x10;
const swm: u8 = 0x20;
pub const adf_all: u8 = 0x07;
const ie_all: u8 = 0x07;
const vbtmnsel: u8 = 0x01;
/// Highest legal VDETLVL (`k_ra8_bkup_vdet_1p75v`).
pub const max_vdet_level: u8 = 5;
/// VDETLVL 110b, the no-switch "initial value" (HUM 12.3.7.3).
const no_switch_lvl: u8 = 0x06;
/// W0C value that clears VBPORF and leaves every other VBTBPSR bit.
const clear_keep: u8 = ~vbporf;

pub const ok: u16 = 0;
pub const invalid_arg: u16 = 0x103;
pub const not_initialized: u16 = 0x10F;
pub const hw_timeout: u16 = 0x203;
pub const null_ptr: u16 = 0x504;

/// `ra8_bkup_config_t`, 3 bytes.
pub const Config = extern struct {
    vdet_level: u8 = 0,
    enable_switch: bool = false,
    enable_backup: bool = false,
};

/// `ra8_bkup_status_t`, 5 bytes. `source` 1 = VCC, 0 = VBATT.
pub const Status = extern struct {
    source: u8 = 0,
    vbatt_r_ok: bool = false,
    por_detected: bool = false,
    tamper_flags: u8 = 0,
    raw_vbtbpsr: u8 = 0,
};

/// `priv_ra8_bkup_internal_rmw8` against a register offset.
pub fn rmw8(regs: anytype, off: usize, mask: u8, enable: bool, unlock: u16) void {
    const live = regs.read8(off);
    regs.prcr(unlock);
    regs.write8(off, if (enable) live | mask else live & ~mask);
    regs.prcr(lock_all);
}

/// Spin until VBPORM reads `want`; false after `iters` reads.
fn waitVbporm(regs: anytype, want: bool, iters: u32) bool {
    var i: u32 = 0;
    while (i < iters) : (i += 1) {
        if (((regs.read8(off_vbtbpsr) & vbporm) != 0) == want) return true;
    }
    return false;
}

fn armVdet(regs: anytype, level: u8) void {
    regs.write8(off_vbtbpcr2, level & lvl_mask);
    regs.write8(off_vbtbpcr2, (level & lvl_mask) | vdete);
}

/// `ra8_bkup_init`.
pub fn init(regs: anytype, c: anytype, cfg_opt: ?*const Config) u16 {
    const cfg = cfg_opt orelse {
        c.err("cfg must not be nullptr");
        return null_ptr;
    };
    if (cfg.vdet_level > max_vdet_level) {
        c.fail("bkup_init: cfg out of range", invalid_arg);
        return invalid_arg;
    }
    regs.prcr(unlock_lpm);
    if (cfg.enable_switch) armVdet(regs, cfg.vdet_level) else regs.write8(off_vbtbpcr1, bpwswstp);
    if (cfg.enable_backup) {
        regs.write8(off_vbtber, vbae);
        c.settle();
    } else {
        regs.write8(off_vbtber, 0);
    }
    regs.write8(off_vbtbpsr, clear_keep);
    regs.write8(off_vbtadsr, 0);
    regs.prcr(lock_all);
    c.setInitialized(true);
    c.info("bkup_init");
    return ok;
}

/// `ra8_bkup_deinit`.
pub fn deinit(regs: anytype, c: anytype) u16 {
    regs.prcr(unlock_lpm);
    regs.write8(off_vbtber, 0);
    regs.write8(off_vbtbpcr1, bpwswstp);
    regs.prcr(lock_all);
    c.setInitialized(false);
    c.info("bkup_deinit");
    return ok;
}

/// `ra8_bkup_cold_start_init` (HUM 12.3.7.1).
pub fn coldStartInit(regs: anytype, c: anytype, level: u8, iters: u32) u16 {
    if (level > max_vdet_level or iters == 0) return invalid_arg;
    if (!waitVbporm(regs, true, iters)) return hw_timeout;
    regs.prcr(unlock_lpm);
    regs.write8(off_vbtbpsr, clear_keep);
    armVdet(regs, level);
    regs.write8(off_vbtbpcr1, 0);
    regs.prcr(lock_all);
    c.setInitialized(true);
    c.info("bkup_cold_start_init");
    return ok;
}

/// `ra8_bkup_warm_start_check` (HUM 12.3.7.2).
pub fn warmStartCheck(regs: anytype, c: anytype, out_opt: ?*bool, iters: u32) u16 {
    const out = out_opt orelse {
        c.err("needs_reinit must not be nullptr");
        return null_ptr;
    };
    if (iters == 0) return invalid_arg;
    if (!waitVbporm(regs, true, iters)) return hw_timeout;
    out.* = (regs.read8(off_vbtbpsr) & vbporf) != 0;
    return ok;
}

/// `ra8_bkup_no_switch_init` (HUM 12.3.7.3).
pub fn noSwitchInit(regs: anytype, c: anytype, iters: u32) u16 {
    if (iters == 0) return invalid_arg;
    regs.prcr(unlock_lpm);
    regs.write8(off_vbtbpcr1, bpwswstp);
    regs.prcr(lock_all);
    if (!waitVbporm(regs, false, iters)) return hw_timeout;
    regs.prcr(unlock_lpm);
    regs.write8(off_vbtbpcr2, no_switch_lvl);
    regs.write8(off_vbtbpsr, clear_keep);
    for ([_]usize{ off_vbtictlr, off_vbtictlr2, off_vbtadsr, off_vbtadcr1, off_vbtadcr2 }) |off| {
        regs.write8(off, 0);
    }
    regs.prcr(lock_all);
    c.setInitialized(true);
    c.info("bkup_no_switch_init");
    return ok;
}

/// `ra8_bkup_get_status`.
pub fn getStatus(regs: anytype, c: anytype, out_opt: ?*Status) u16 {
    const out = out_opt orelse {
        c.err("out must not be nullptr");
        return null_ptr;
    };
    const bpsr = regs.read8(off_vbtbpsr);
    const adsr = regs.read8(off_vbtadsr);
    out.raw_vbtbpsr = bpsr;
    out.source = if ((bpsr & swm) != 0) 1 else 0;
    out.vbatt_r_ok = (bpsr & vbporm) != 0;
    out.por_detected = (bpsr & vbporf) != 0;
    out.tamper_flags = adsr & adf_all;
    return ok;
}

/// `ra8_bkup_clear_status`: W0C on VBPORF and the selected VBTADFn.
pub fn clearStatus(regs: anytype, mask: u8) u16 {
    if ((mask & vbporf) != 0) rmw8(regs, off_vbtbpsr, vbporf, false, unlock_lpm);
    const adf = mask & adf_all;
    if (adf != 0) rmw8(regs, off_vbtadsr, adf, false, unlock_lpm);
    return ok;
}

pub fn wordOff(index: u8) usize {
    return off_vbtbkr0 + @as(usize, index) * 4;
}

pub fn byteOff(index: u16) usize {
    return off_vbtbkr0 + index;
}

/// `ra8_bkup_read_word`.
pub fn readWord(regs: anytype, c: anytype, index: u8, out_opt: ?*u32) u16 {
    const out = out_opt orelse {
        c.err("out must not be nullptr");
        return null_ptr;
    };
    if (index >= word_count) return invalid_arg;
    out.* = regs.read32(wordOff(index));
    return ok;
}

/// `ra8_bkup_write_word`; VBTBKRn drops unprotected stores.
pub fn writeWord(regs: anytype, index: u8, value: u32) u16 {
    if (index >= word_count) return invalid_arg;
    regs.prcr(unlock_lpm);
    regs.write32(wordOff(index), value);
    regs.prcr(lock_all);
    return ok;
}

/// `ra8_bkup_read_byte`.
pub fn readByte(regs: anytype, c: anytype, index: u16, out_opt: ?*u8) u16 {
    const out = out_opt orelse {
        c.err("out must not be nullptr");
        return null_ptr;
    };
    if (index >= reg_count) return invalid_arg;
    out.* = regs.read8(byteOff(index));
    return ok;
}

/// `ra8_bkup_write_byte`.
pub fn writeByte(regs: anytype, index: u16, value: u8) u16 {
    if (index >= reg_count) return invalid_arg;
    regs.prcr(unlock_lpm);
    regs.write8(byteOff(index), value);
    regs.prcr(lock_all);
    return ok;
}

/// `ra8_bkup_zero_all`: one PRC1 window over all 128 bytes.
pub fn zeroAll(regs: anytype) u16 {
    regs.prcr(unlock_lpm);
    var i: u16 = 0;
    while (i < reg_count) : (i += 1) regs.write8(byteOff(i), 0);
    regs.prcr(lock_all);
    return ok;
}

/// `ra8_bkup_set_voltage_monitor`; VBATTMNSELR sits under PRC3.
pub fn setVoltageMonitor(regs: anytype, enable: bool) u16 {
    rmw8(regs, off_vbattmnselr, vbtmnsel, enable, unlock_pvd);
    return ok;
}

/// `ra8_bkup_get_voltage_monitor_enabled`.
pub fn getVoltageMonitor(regs: anytype, c: anytype, out_opt: ?*bool) u16 {
    const out = out_opt orelse {
        c.err("enabled_out must not be nullptr");
        return null_ptr;
    };
    out.* = (regs.read8(off_vbattmnselr) & vbtmnsel) != 0;
    return ok;
}

/// `ra8_bkup_isr_handle`: clear and dispatch the enabled flags that fired.
pub fn isrHandle(regs: anytype, c: anytype) u16 {
    if (!c.isInitialized()) return not_initialized;
    const fired = regs.read8(off_vbtadsr) & adf_all & (regs.read8(off_vbtadcr1) & ie_all);
    if (fired != 0) {
        rmw8(regs, off_vbtadsr, fired, false, unlock_lpm);
        c.dispatch(fired);
    }
    return ok;
}
