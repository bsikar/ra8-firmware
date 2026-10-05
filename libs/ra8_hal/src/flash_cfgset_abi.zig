//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the start-up area and configuration-set write moved out of
//! ra8_flash_config.c (RA8FW-808). Words are in internal/flash_cfgset.zig.

const common = @import("abi_common.zig");
const rt = @import("flash_rt.zig");
const cfg = @import("internal/flash_cfgset.zig");

const ok = common.k_ra8_ok;

extern fn ra8_flash_enter_pe_mode() u16;
extern fn ra8_flash_exit_pe_mode() u16;
extern fn priv_ra8_flash_internal_maci_cmd8(byte: u8) void;
extern fn priv_ra8_flash_internal_maci_cmd16(half: u16) void;
extern fn priv_ra8_flash_internal_wait_mrdy(limit: u32) u16;

fn read32(a: usize) u32 {
    return @as(*volatile u32, @ptrFromInt(a)).*;
}

export fn ra8_flash_set_startup_area(target: u8, temporary: bool) u16 {
    if (target > cfg.startup_max) return common.k_ra8_err_invalid_arg;
    var err = ra8_flash_enter_pe_mode();
    if (err != ok) return err;
    if (temporary) {
        const msuacr: *volatile u16 = @ptrFromInt(cfg.reg(cfg.off_msuacr));
        msuacr.* = cfg.msuacrWord(target);
    } else {
        const words = cfg.startupWords(target);
        err = ra8_flash_config_set_write(cfg.startup_addr, &words);
    }
    const exit_err = ra8_flash_exit_pe_mode();
    return if (err != ok) err else exit_err;
}

export fn ra8_flash_get_startup_area(out_btflg: ?*u8, out_fspr: ?*u8) u16 {
    if (!rt.present(out_btflg, "out_btflg must not be nullptr")) return common.k_ra8_err_null_ptr;
    if (!rt.present(out_fspr, "out_fspr must not be nullptr")) return common.k_ra8_err_null_ptr;
    const flags = cfg.startupFlags(read32(cfg.reg(cfg.off_msuasmon)));
    out_btflg.?.* = flags.btflg;
    out_fspr.?.* = flags.fspr;
    return ok;
}

export fn ra8_flash_config_set_write(target_addr: u32, words: ?[*]const u16) u16 {
    if (!rt.present(words, "words must not be nullptr")) return common.k_ra8_err_null_ptr;
    const r = cfg.region(target_addr);
    if (r == .none) return common.k_ra8_err_invalid_arg;
    @as(*volatile u32, @ptrFromInt(cfg.reg(cfg.off_msaddr))).* = target_addr;
    priv_ra8_flash_internal_maci_cmd8(cfg.opener(r));
    priv_ra8_flash_internal_maci_cmd8(cfg.cmd_word_count);
    for (words.?[0..cfg.word_count]) |w| priv_ra8_flash_internal_maci_cmd16(w);
    priv_ra8_flash_internal_maci_cmd8(cfg.cmd_final);
    const err = priv_ra8_flash_internal_wait_mrdy(cfg.maci_spin_limit);
    if (err != ok) return err;
    if (read32(cfg.reg(cfg.off_mstatr)) & cfg.mstatr_any_err != 0) return common.k_ra8_err_hw_error;
    return ok;
}
