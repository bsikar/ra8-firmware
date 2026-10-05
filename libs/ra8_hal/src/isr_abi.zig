//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the ra8_isr.h allocator (RA8FW-760), replacing ra8_isr.c. Logic
//! is in internal/isr.zig. The fixed NVIC and ICU addresses match the C; on
//! the host, ra8_fake_mmap backs both windows.

const common = @import("abi_common.zig");
const isr = @import("internal/isr.zig");

const tag = "ISR";

const nvic_iser: usize = 0xE000_E100;
const nvic_icer: usize = 0xE000_E180;
const nvic_icpr: usize = 0xE000_E280;
const nvic_ipr: usize = 0xE000_E400;
const prio_shift: u3 = 4;
const icu_ielsr0: usize = 0x4000_6000 + 0x6300;

var pool: isr.Pool = [_]isr.Slot{.{}} ** isr.slot_count;

fn nvicWord(base: usize, n: u16) void {
    const reg: *volatile u32 = @ptrFromInt(base + @as(usize, n / 32) * 4);
    reg.* = @as(u32, 1) << @intCast(n % 32);
}

const Hw = struct {
    pub fn nvicEnable(_: Hw, n: u16) void {
        nvicWord(nvic_iser, n);
    }
    pub fn nvicDisable(_: Hw, n: u16) void {
        nvicWord(nvic_icer, n);
    }
    pub fn nvicClearPending(_: Hw, n: u16) void {
        nvicWord(nvic_icpr, n);
    }
    pub fn nvicSetPriority(_: Hw, n: u16, prio: u8) void {
        const reg: *volatile u8 = @ptrFromInt(nvic_ipr + n);
        reg.* = prio << prio_shift;
    }
    fn ielsr(slot: u16) *volatile u32 {
        return @ptrFromInt(icu_ielsr0 + @as(usize, slot) * 4);
    }
    pub fn ielsrRead(_: Hw, slot: u16) u32 {
        return ielsr(slot).*;
    }
    pub fn ielsrWrite(_: Hw, slot: u16, value: u32) void {
        ielsr(slot).* = value;
    }
};

export fn ra8_isr_init() u16 {
    common.ra8_log_emit_info(tag, "ra8_isr_init");
    isr.init(&pool, Hw{});
    return common.k_ra8_ok;
}

export fn ra8_isr_register(event: u16, handler: ?isr.Handler, ctx: ?*anyopaque, priority: u8, out_slot: ?*u16) u16 {
    const h = handler orelse {
        common.ra8_log_emit_error(tag, "handler must not be NULL");
        return common.k_ra8_err_null_ptr;
    };
    const err = isr.register(&pool, Hw{}, event, h, ctx, priority, out_slot);
    if (err == isr.err_no_mem) common.ra8_log_emit_error(tag, "no free slot");
    return err;
}

export fn ra8_isr_unregister(event: u16) u16 {
    return isr.unregister(&pool, Hw{}, event);
}

export fn ra8_isr_dispatch(slot: u16) void {
    isr.dispatch(&pool, Hw{}, slot);
}

export fn ra8_isr_set_priority(event: u16, priority: u8) u16 {
    return isr.setPriority(&pool, Hw{}, event, priority);
}

export fn ra8_isr_lookup_slot(event: u16, out_slot: ?*u16) u16 {
    const o = out_slot orelse {
        common.ra8_log_emit_error(tag, "out_slot must not be NULL");
        return common.k_ra8_err_null_ptr;
    };
    o.* = isr.findEvent(&pool, event);
    return common.k_ra8_ok;
}

export fn ra8_isr_set_dtc(slot: u16, enable: bool) u16 {
    return isr.setDtc(&pool, Hw{}, slot, enable);
}
