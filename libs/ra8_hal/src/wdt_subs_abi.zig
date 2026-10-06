//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the WDT subscriber table, ra8_wdt_dispatch, ra8_wdt_deinit,
//! the NMI wiring and the sleep-stop calls (RA8FW-890, was ra8_wdt.c).
//! Same log text and error codes as the C. HUM Ch 27.2.3 "WDTSR" p 1260,
//! Ch 27.2.5 "WDTCSTPR" p 1262, Ch 14.2.14/15 "NMIER"/"NMICLR" pp 542-544.

const common = @import("abi_common.zig");
const subs = @import("internal/wdt_subs.zig");

const tag = "WDT";
const k_ra8_err_no_mem: u16 = 0x102;
const k_ra8_err_not_found: u16 = 0x106;

const wdt0_base: usize = 0x4020_2600;
const off_wdtsr: usize = 0x04;
const off_wdtcstpr: usize = 0x08;
const cstpr_slcstp: u8 = 0x80;
const status_all: u16 = 0xC000; // UNDFF bit 14 | REFEF bit 15
const nmier_wdten: u32 = 0x2;

extern fn ra8_icu_nmi_enable(mask: u32) u16;
extern fn ra8_icu_nmi_disable(mask: u32) u16;
extern fn ra8_icu_nmi_clear(mask: u32) u16;

var table: subs.Table = .{};

fn wdtsr() *volatile u16 {
    return @ptrFromInt(wdt0_base + off_wdtsr);
}

fn wdtcstpr() *volatile u8 {
    return @ptrFromInt(wdt0_base + off_wdtcstpr);
}

export fn ra8_wdt_attach_handler(func: ?subs.EventFn, ctx: ?*anyopaque) u16 {
    table.attach(func, ctx);
    return common.k_ra8_ok;
}

export fn ra8_wdt_subscribe(func: ?subs.EventFn, ctx: ?*anyopaque, out_slot: ?*u8) u16 {
    const f = func orelse {
        common.ra8_log_emit_error(tag, "fn must not be nullptr");
        return common.k_ra8_err_null_ptr;
    };
    const slot = table.subscribe(f, ctx) catch {
        common.ra8_log_emit_error(tag, "wdt_subscribe table full");
        return k_ra8_err_no_mem;
    };
    if (out_slot) |out| out.* = slot;
    return common.k_ra8_ok;
}

export fn ra8_wdt_unsubscribe(slot: u8) u16 {
    table.unsubscribe(slot) catch |err| return switch (err) {
        error.BadSlot => common.k_ra8_err_invalid_arg,
        else => k_ra8_err_not_found,
    };
    return common.k_ra8_ok;
}

export fn ra8_wdt_subscriber_count() u8 {
    return table.count();
}

/// Snapshot the flags, clear them (write-0-to-clear), then notify.
export fn ra8_wdt_dispatch() void {
    const reg = wdtsr();
    const mask = reg.* & status_all;
    reg.* = reg.* & ~mask;
    table.notify(mask);
}

/// The WDT cannot be disarmed once started; halting it in Sleep is the
/// strongest "off" available.
export fn ra8_wdt_deinit() u16 {
    wdtcstpr().* = cstpr_slcstp;
    table.clear();
    common.ra8_log_emit_info(tag, "wdt_deinit (sleep-stop set)");
    return common.k_ra8_ok;
}

/// Clear any stale WDT NMI flag before unmasking the source.
export fn ra8_wdt_install_nmi() u16 {
    const rc = ra8_icu_nmi_clear(nmier_wdten);
    if (rc != common.k_ra8_ok) return rc;
    return ra8_icu_nmi_enable(nmier_wdten);
}

export fn ra8_wdt_uninstall_nmi() u16 {
    const rc = ra8_icu_nmi_disable(nmier_wdten);
    if (rc != common.k_ra8_ok) return rc;
    return ra8_icu_nmi_clear(nmier_wdten);
}

export fn ra8_wdt_enter_stop() u16 {
    wdtcstpr().* = cstpr_slcstp;
    return common.k_ra8_ok;
}

export fn ra8_wdt_exit_stop() u16 {
    wdtcstpr().* = 0;
    return common.k_ra8_ok;
}
