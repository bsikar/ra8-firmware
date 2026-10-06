//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for ra8_rtc_init and ra8_rtc_clock_init (RA8FW-855). Logic
//! lives in internal/rtc_init.zig. Waits go through
//! priv_ra8_rtc_internal_wait_bit (src/rtc_stop_abi.zig).

const builtin = @import("builtin");
const common = @import("abi_common.zig");
const r = @import("internal/rtc_init.zig");

const tag = "RTC";
/// `k_ra8_rtc_base_addr` and `k_ra8_system_base_addr`.
const rtc_base: usize = 0x40202000;
const sys_base: usize = 0x4001E000;
const hosted = builtin.os.tag != .freestanding;

extern fn priv_ra8_rtc_internal_wait_bit(reg: *volatile u8, mask: u8, expect: u8) void;
extern fn ra8_delay_ms(ms: u32) void;

const seam = struct {
    extern fn ra8_fake_mmio_wait_eval(reg: *const volatile anyopaque, iter: u32, real_cond: bool) bool;
};

const Hw = struct {
    pub fn wait(_: Hw, reg: *volatile u8, mask: u8, expect: u8) void {
        priv_ra8_rtc_internal_wait_bit(reg, mask, expect);
    }
    pub fn delay(_: Hw, ms: u32) void {
        ra8_delay_ms(ms);
    }
    pub fn prcr(_: Hw, value: u16) void {
        const p: *volatile u16 = @ptrFromInt(sys_base + 0x3FA);
        p.* = value;
    }
    pub fn running(_: Hw, reg: *volatile u8, stop_mask: u8) bool {
        const cond = (reg.* & stop_mask) == 0;
        return if (hosted) seam.ra8_fake_mmio_wait_eval(reg, 0, cond) else cond;
    }
    pub fn info(_: Hw, msg: [*:0]const u8) void {
        common.ra8_log_emit_info(tag, msg);
    }
    pub fn infoVal(_: Hw, msg: [*:0]const u8, value: u32) void {
        common.ra8_log_emit_info_val(tag, msg, value);
    }
    pub fn err(_: Hw, msg: [*:0]const u8) void {
        common.ra8_log_emit_error(tag, msg);
    }
};

fn view() r.View {
    return .{
        .rcr1 = @ptrFromInt(rtc_base + 0x22),
        .rcr2 = @ptrFromInt(rtc_base + 0x24),
        .rcr4 = @ptrFromInt(rtc_base + 0x28),
        .rfrh = @ptrFromInt(rtc_base + 0x2A),
        .rfrl = @ptrFromInt(rtc_base + 0x2C),
        .lococr = @ptrFromInt(sys_base + 0x400),
        .sosccr = @ptrFromInt(sys_base + 0xC00),
        .somcr = @ptrFromInt(sys_base + 0xC01),
    };
}

export fn ra8_rtc_init() u16 {
    return r.init(Hw{}, view());
}

export fn ra8_rtc_clock_init(src: u8) u16 {
    return r.clockInit(Hw{}, view(), src);
}
