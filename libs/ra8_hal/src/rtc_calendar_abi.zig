//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for ra8_rtc_set, ra8_rtc_get and ra8_rtc_set_alarm (RA8FW-854).
//! Logic lives in internal/rtc_calendar.zig. Waits go through
//! priv_ra8_rtc_internal_wait_bit (src/rtc_stop_abi.zig).

const common = @import("abi_common.zig");
const c = @import("internal/rtc_calendar.zig");

const tag = "RTC";
/// `k_ra8_rtc_base_addr` (ra8_rtc_regs.h); RCR2 +0x24.
const rtc_base: usize = 0x40202000;

extern fn priv_ra8_rtc_internal_wait_bit(reg: *volatile u8, mask: u8, expect: u8) void;

const Hw = struct {
    pub fn wait(_: Hw, reg: *volatile u8, mask: u8, expect: u8) void {
        priv_ra8_rtc_internal_wait_bit(reg, mask, expect);
    }
    pub fn infoVal(_: Hw, msg: [*:0]const u8, value: u32) void {
        common.ra8_log_emit_info_val(tag, msg, value);
    }
};

fn cal() *volatile c.Cal {
    return @ptrFromInt(rtc_base);
}

fn nullPtr(msg: [*:0]const u8) u16 {
    common.ra8_log_emit_error(tag, msg);
    return common.k_ra8_err_null_ptr;
}

export fn ra8_rtc_set(dt: ?*const c.Datetime) u16 {
    const d = dt orelse return nullPtr("dt must not be nullptr");
    return c.set(Hw{}, cal(), @ptrFromInt(rtc_base + 0x24), d);
}

export fn ra8_rtc_get(out: ?*c.Datetime) u16 {
    const o = out orelse return nullPtr("out must not be nullptr");
    c.get(cal(), o);
    return common.k_ra8_ok;
}

export fn ra8_rtc_set_alarm(alarm: ?*const c.Datetime) u16 {
    const a = alarm orelse return nullPtr("alarm must not be nullptr");
    return c.setAlarm(cal(), a);
}
