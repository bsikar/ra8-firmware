//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the ra8_sram.h control calls (RA8FW-798), replacing ra8_sram.c.
//! The logic is in internal/sram.zig. The security setters and handler
//! table are in sram_security_abi.zig.

const common = @import("abi_common.zig");
const sram = @import("internal/sram.zig");

const tag = "SRAM";
const ok = sram.codes.ok;
const invalid_arg = sram.codes.invalid_arg;

const ErrFn = *const fn (ctx: ?*anyopaque, bank: u8, is_2bit: bool, err_addr: usize) callconv(.C) void;

extern fn ra8_mstp_enable(id: u16) u16;
extern fn ra8_mstp_disable(id: u16) u16;
extern var g_sram_on_error: ?ErrFn;
extern var g_sram_on_error_ctx: ?*anyopaque;
extern var g_sram_on_error_bank: [sram.bank_count]?ErrFn;
extern var g_sram_on_error_bank_ctx: [sram.bank_count]?*anyopaque;

const Hw = struct {
    fn at(comptime T: type, a: usize) *volatile T {
        return @ptrFromInt(a);
    }
    pub fn read16(_: Hw, a: usize) u16 {
        return at(u16, a).*;
    }
    pub fn read32(_: Hw, a: usize) u32 {
        return at(u32, a).*;
    }
    pub fn read64(_: Hw, a: usize) u64 {
        return at(u64, a).*;
    }
    pub fn write8(_: Hw, a: usize, v: u8) void {
        at(u8, a).* = v;
    }
    pub fn write16(_: Hw, a: usize, v: u16) void {
        at(u16, a).* = v;
    }
    pub fn write32(_: Hw, a: usize, v: u32) void {
        at(u32, a).* = v;
    }
    pub fn write64(_: Hw, a: usize, v: u64) void {
        at(u64, a).* = v;
    }
};

const hw = Hw{};

/// RA8_RETURN_ON_ERROR: the message, then the code.
fn failed(rc: u16, msg: [*:0]const u8) bool {
    if (rc == ok) return false;
    common.ra8_log_emit_error(tag, msg);
    common.ra8_log_emit_error_val(tag, "Error", rc);
    return true;
}

fn nullFail(msg: [*:0]const u8) u16 {
    common.ra8_log_emit_error(tag, msg);
    return sram.codes.null_ptr;
}

fn validateAndUngate(c: *const sram.Config) u16 {
    for (c.banks, 0..) |b, i| {
        const rc: u16 = if (sram.bankCfgOk(b, @intCast(i))) ok else invalid_arg;
        if (failed(rc, "ra8_sram_init: bad bank cfg")) return rc;
    }
    for (0..sram.bank_count) |i| {
        const rc = ra8_mstp_enable(@intCast(i));
        if (failed(rc, "ra8_sram_init: mstp enable")) return rc;
    }
    return ok;
}

export fn ra8_sram_init(cfg: ?*const sram.Config) u16 {
    const c = cfg orelse return nullFail("cfg must not be nullptr");
    const rc = validateAndUngate(c);
    if (failed(rc, "ra8_sram_init: validate/ungate")) return rc;
    if (c.apply_security) sram.applySecurity(hw, c.security);
    sram.applyBanks(hw, c.*);
    hw.write16(sram.reg.esclr, sram.err_all);
    common.ra8_log_emit_info(tag, "ra8_sram_init done");
    return ok;
}

export fn ra8_sram_deinit() u16 {
    sram.deinitRegs(hw);
    for (0..sram.bank_count) |i| _ = ra8_mstp_disable(@intCast(i));
    g_sram_on_error = null;
    g_sram_on_error_ctx = null;
    for (0..sram.bank_count) |i| {
        g_sram_on_error_bank[i] = null;
        g_sram_on_error_bank_ctx[i] = null;
    }
    return ok;
}

export fn ra8_sram_enter_stop(bank: u8) u16 {
    if (!sram.bankOk(bank)) return invalid_arg;
    return ra8_mstp_disable(bank);
}

export fn ra8_sram_exit_stop(bank: u8) u16 {
    if (!sram.bankOk(bank)) return invalid_arg;
    return ra8_mstp_enable(bank);
}

export fn ra8_sram_set_mode(bank: u8, cfg: ?*const sram.BankCfg) u16 {
    const c = cfg orelse return nullFail("cfg must not be nullptr");
    if (!sram.bankOk(bank)) return invalid_arg;
    if (!sram.bankCfgOk(c.*, bank)) {
        _ = failed(invalid_arg, "ra8_sram_set_mode: bad bank cfg");
        return invalid_arg;
    }
    sram.writeEccrgn(hw, bank, c.eccrgn);
    sram.writeCr(hw, bank, sram.encodeCr(c.*));
    return ok;
}

export fn ra8_sram_set_eccrgn(bank: u8, region: u8) u16 {
    if (!sram.bankOk(bank) or region > sram.maxRgn(bank)) return invalid_arg;
    sram.writeEccrgn(hw, bank, region);
    return ok;
}

export fn ra8_sram_set_wait_state_for_clock(iclk_hz: u32, iclk_max_hz: u32) u16 {
    if (iclk_hz == 0 or iclk_max_hz == 0) return invalid_arg;
    sram.writeWtsc(hw, sram.wtscFor(iclk_hz, iclk_max_hz));
    return ok;
}

export fn ra8_sram_get_status(out: ?*sram.Status) u16 {
    const o = out orelse return nullFail("out must not be nullptr");
    o.* = sram.readStatus(hw);
    return ok;
}

export fn ra8_sram_clear_status(esr_mask: u16) u16 {
    if (esr_mask & ~sram.err_all != 0) return invalid_arg;
    hw.write16(sram.reg.esclr, esr_mask);
    return ok;
}

export fn ra8_sram_clear_address(bank: u8, slot: u8) u16 {
    if (!sram.bankOk(bank) or slot > 1) return invalid_arg;
    hw.write16(sram.reg.esclr, sram.errBit(bank, slot));
    return ok;
}

export fn ra8_sram_zero_init_bank(bank: u8) u16 {
    if (!sram.bankOk(bank)) return invalid_arg;
    sram.zeroInit(hw, bank);
    return ok;
}

export fn ra8_sram_self_test(bank: u8, probe_offset: u32, inject_two_bit: bool, out_caught: ?*bool) u16 {
    const o = out_caught orelse return nullFail("out_caught must not be nullptr");
    if (!sram.bankOk(bank)) return invalid_arg;
    if (probe_offset & 7 != 0 or probe_offset >= sram.bankSize(bank)) return invalid_arg;
    o.* = false;
    o.* = sram.selfTest(hw, bank, probe_offset, inject_two_bit);
    return ok;
}

export fn ra8_sram_get_bank_info(bank: u8, out: ?*sram.BankInfo) u16 {
    const o = out orelse return nullFail("out must not be nullptr");
    if (!sram.bankOk(bank)) return invalid_arg;
    o.* = sram.bankInfo(bank);
    return ok;
}
