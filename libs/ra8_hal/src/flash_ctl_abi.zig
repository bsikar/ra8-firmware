//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the MRAM control calls moved out of ra8_flash_config.c
//! (RA8FW-806). Register words are in internal/flash_ctl.zig.

const common = @import("abi_common.zig");
const rt = @import("flash_rt.zig");
const ctl = @import("internal/flash_ctl.zig");

const ok = common.k_ra8_ok;
const null_ptr = common.k_ra8_err_null_ptr;

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
    pub fn write16(_: Hw, a: usize, v: u16) void {
        @as(*volatile u16, @ptrFromInt(a)).* = v;
    }
};

const hw = Hw{};

export fn ra8_flash_zeroize_huk() u16 {
    if (!rt.ready("zeroize before init")) return common.k_ra8_err_not_initialized;
    return if (ctl.zeroize(hw, ctl.zeroize_spin)) ok else common.k_ra8_err_hw_timeout;
}

export fn ra8_flash_set_security_attribution(new_msar: u16) u16 {
    hw.write16(ctl.reg(ctl.off_msar), new_msar);
    return ok;
}

export fn ra8_flash_set_ecc_encoder_enable(enable: bool) u16 {
    hw.write16(ctl.reg(ctl.off_mrceecc), ctl.encoderWord(enable));
    return ok;
}

export fn ra8_flash_set_ecc_decoder_enable(enable: bool) u16 {
    hw.write16(ctl.reg(ctl.off_mrcdecc), ctl.decoderWord(enable));
    return ok;
}

export fn ra8_flash_get_ecc_error_addr(code_ted: ?*u32, code_dec: ?*u32, extra_ted: ?*u32, extra_dec: ?*u32) u16 {
    if (!rt.present(code_ted, "out_code_ted null")) return null_ptr;
    if (!rt.present(code_dec, "out_code_dec null")) return null_ptr;
    if (!rt.present(extra_ted, "out_extra_ted null")) return null_ptr;
    if (!rt.present(extra_dec, "out_extra_dec null")) return null_ptr;
    code_ted.?.* = hw.read32(ctl.reg(ctl.off_mrcrtea));
    code_dec.?.* = hw.read32(ctl.reg(ctl.off_mrcrdea));
    extra_ted.?.* = hw.read32(ctl.reg(ctl.off_mrertea));
    extra_dec.?.* = hw.read32(ctl.reg(ctl.off_mrerdea));
    return ok;
}

export fn ra8_flash_get_program_error_addr(out_addr: ?*u32) u16 {
    const out = out_addr orelse {
        _ = rt.present(null, "out_addr must not be nullptr");
        return null_ptr;
    };
    out.* = hw.read32(ctl.reg(ctl.off_mrcpea));
    return ok;
}

export fn ra8_flash_set_update_transfer(list_select: u8) u16 {
    if (list_select > ctl.max_list_select) return common.k_ra8_err_invalid_arg;
    hw.write8(ctl.reg(ctl.off_mctrlsr), list_select & ctl.mctrlsr_list_mask);
    hw.write16(ctl.reg(ctl.off_mctrcntr), ctl.mctrcntr_start);
    return ok;
}

export fn ra8_flash_get_update_status(busy: ?*u8, done: ?*u8, err: ?*u8) u16 {
    if (!rt.present(busy, "out_busy null")) return null_ptr;
    if (!rt.present(done, "out_done null")) return null_ptr;
    if (!rt.present(err, "out_err null")) return null_ptr;
    const s = ctl.status(hw.read16(ctl.reg(ctl.off_mctrstatr)));
    busy.?.* = s.busy;
    done.?.* = s.done;
    err.?.* = s.err;
    return ok;
}
