//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for ra8_flash_arc_increment / ra8_flash_arc_read (RA8FW-802),
//! moved out of ra8_flash_config.c. The logic is in internal/flash_arc.zig.
//! ra8_flash.c still owns the runtime state (flash_rt.zig), PE mode and
//! the MACI helpers, reached as externs.

const common = @import("abi_common.zig");
const arc = @import("internal/flash_arc.zig");
const rt = @import("flash_rt.zig");

const ok = common.k_ra8_ok;
const invalid_arg = common.k_ra8_err_invalid_arg;

extern fn ra8_flash_enter_pe_mode() u16;
extern fn ra8_flash_exit_pe_mode() u16;
extern fn priv_ra8_flash_internal_maci_cmd8(byte: u8) void;
extern fn priv_ra8_flash_internal_wait_mrdy(limit: u32) u16;

const Hw = struct {
    pub fn read8(_: Hw, a: usize) u8 {
        return @as(*volatile u8, @ptrFromInt(a)).*;
    }
    pub fn read16(_: Hw, a: usize) u16 {
        return @as(*volatile u16, @ptrFromInt(a)).*;
    }
    pub fn read32(_: Hw, a: usize) u32 {
        return @as(*volatile u32, @ptrFromInt(a)).*;
    }
    pub fn write8(_: Hw, a: usize, v: u8) void {
        @as(*volatile u8, @ptrFromInt(a)).* = v;
    }
};

const hw = Hw{};

fn notReady(msg: [*:0]const u8) bool {
    return !rt.ready(msg);
}

/// MCNTSELR, the counter command, the 0xD0 trailer, MRDY, then CMDLK.
fn counterCmd(sel: u8, cmd: u8) u16 {
    hw.write8(arc.mram_base + arc.off_mcntselr, sel & arc.mcntselr_mask);
    priv_ra8_flash_internal_maci_cmd8(cmd);
    priv_ra8_flash_internal_maci_cmd8(arc.cmd_final);
    const err = priv_ra8_flash_internal_wait_mrdy(arc.spin_limit);
    if (err != ok) return err;
    if (hw.read8(arc.mram_base + arc.off_mastat) & arc.mastat_cmdlk != 0) return common.k_ra8_err_hw_error;
    return ok;
}

/// Read a counter; for OEMBL the caller already holds PE mode.
fn readLocked(id: u8, out: *u32) u16 {
    if (id == arc.arc_oembl) {
        const err = counterCmd(arc.mcntselr(id), arc.cmd_read);
        if (err != ok) return err;
        const lo = hw.read32(arc.mram_base + arc.off_mcntdtr0);
        const hi = hw.read32(arc.mram_base + arc.off_mcntdtr1);
        out.* = @popCount(lo) + @as(u32, @popCount(hi));
        return ok;
    }
    out.* = arc.ofsCount(hw, id);
    return ok;
}

export fn ra8_flash_arc_increment(counter: u8) u16 {
    if (counter >= arc.arc_count) return invalid_arg;
    if (notReady("arc_inc before init")) return common.k_ra8_err_not_initialized;
    var err = ra8_flash_enter_pe_mode();
    if (err != ok) return err;
    var cur: u32 = 0;
    err = readLocked(counter, &cur);
    if (err == ok) {
        const max = arc.maxCount(counter, hw.read16(arc.arccs_addr));
        err = if (cur + 1 > max) common.k_ra8_err_out_of_range else counterCmd(arc.mcntselr(counter), arc.cmd_increment);
    }
    const exit_err = ra8_flash_exit_pe_mode();
    return if (err == ok) exit_err else err;
}

export fn ra8_flash_arc_read(counter: u8, out_count: ?*u32) u16 {
    const out = out_count orelse {
        _ = rt.present(null, "out_count must not be nullptr");
        return common.k_ra8_err_null_ptr;
    };
    if (counter >= arc.arc_count) return invalid_arg;
    if (notReady("arc_read before init")) return common.k_ra8_err_not_initialized;
    if (counter != arc.arc_oembl) return readLocked(counter, out);
    const err = ra8_flash_enter_pe_mode();
    if (err != ok) return err;
    const r = readLocked(counter, out);
    const exit_err = ra8_flash_exit_pe_mode();
    return if (r != ok) r else exit_err;
}
