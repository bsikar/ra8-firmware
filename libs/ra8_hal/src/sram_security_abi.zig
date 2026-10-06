//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI exports for the SRAM attribution setters and ECC error handlers
//! (internal/sram_security.zig, RA8FW-554). Built as its own object in
//! libra8_hal.a (RA8FW-542). The g_sram_on_error* globals keep their C
//! names because src/ra8_sram_internal.h declares them.

const common = @import("abi_common.zig");
const sec = @import("internal/sram_security.zig");
const k_ra8_ok = common.k_ra8_ok;
const k_ra8_err_invalid_arg = common.k_ra8_err_invalid_arg;
const k_ra8_err_null_ptr = common.k_ra8_err_null_ptr;
const ra8_log_emit_error = common.ra8_log_emit_error;

/// `ra8_sram_error_fn_t`.
const ErrFn = *const fn (ctx: ?*anyopaque, bank: u8, is_2bit: bool, err_addr: usize) callconv(.C) void;

extern fn ra8_sram_get_status(out: *sec.Status) u16;

const tag = "SRAM";
const none: [sec.bank_count]?ErrFn = @splat(null);
const no_ctx: [sec.bank_count]?*anyopaque = @splat(null);

export var g_sram_on_error: ?ErrFn = null;
export var g_sram_on_error_ctx: ?*anyopaque = null;
export var g_sram_on_error_bank: [sec.bank_count]?ErrFn = none;
export var g_sram_on_error_bank_ctx: [sec.bank_count]?*anyopaque = no_ctx;

fn cpscu() *volatile sec.Cpscu {
    return @ptrFromInt(sec.cpscu_base);
}

/// `ra8_err_t ra8_sram_set_security(uint32_t sa_mask)`.
export fn ra8_sram_set_security(sa_mask: u32) u16 {
    sec.setSecurity(cpscu(), sa_mask) catch return k_ra8_err_invalid_arg;
    return k_ra8_ok;
}

/// `ra8_err_t ra8_sram_set_ecc_security(bool non_secure)`.
export fn ra8_sram_set_ecc_security(non_secure: bool) u16 {
    sec.setEccSecurity(cpscu(), non_secure);
    return k_ra8_ok;
}

/// `ra8_err_t ra8_sram_set_boundary(uint8_t bank, uint32_t offset)`.
export fn ra8_sram_set_boundary(bank: u8, offset: u32) u16 {
    sec.setBoundary(cpscu(), bank, offset) catch return k_ra8_err_invalid_arg;
    return k_ra8_ok;
}

/// `ra8_err_t ra8_sram_attach_handler(ra8_sram_error_fn_t fn, void* ctx)`.
export fn ra8_sram_attach_handler(func: ?ErrFn, ctx: ?*anyopaque) u16 {
    const f = func orelse {
        ra8_log_emit_error(tag, "fn must not be nullptr");
        return k_ra8_err_null_ptr;
    };
    g_sram_on_error = f;
    g_sram_on_error_ctx = ctx;
    return k_ra8_ok;
}

/// `ra8_err_t ra8_sram_attach_bank_handler(uint8_t bank, ra8_sram_error_fn_t fn, void* ctx)`.
export fn ra8_sram_attach_bank_handler(bank: u8, func: ?ErrFn, ctx: ?*anyopaque) u16 {
    const f = func orelse {
        ra8_log_emit_error(tag, "fn must not be nullptr");
        return k_ra8_err_null_ptr;
    };
    if (bank >= sec.bank_count) return k_ra8_err_invalid_arg;
    g_sram_on_error_bank[bank] = f;
    g_sram_on_error_bank_ctx[bank] = ctx;
    return k_ra8_ok;
}

/// `void ra8_sram_dispatch(uint8_t bank, bool is_2bit, uintptr_t err_addr)`:
/// the global handler first, then the bank's.
export fn ra8_sram_dispatch(bank: u8, is_2bit: bool, err_addr: usize) void {
    if (bank >= sec.bank_count) return;
    const global_fn = g_sram_on_error;
    const global_ctx = g_sram_on_error_ctx;
    if (global_fn) |f| f(global_ctx, bank, is_2bit, err_addr);
    const bank_fn = g_sram_on_error_bank[bank];
    const bank_ctx = g_sram_on_error_bank_ctx[bank];
    if (bank_fn) |f| f(bank_ctx, bank, is_2bit, err_addr);
}

const Dispatcher = struct {
    pub fn fire(_: Dispatcher, bank: u8, is_2bit: bool, addr: usize) void {
        ra8_sram_dispatch(bank, is_2bit, addr);
    }
};

/// `uint16_t ra8_sram_dispatch_from_esr(ra8_sram_status_t* out_status)`.
export fn ra8_sram_dispatch_from_esr(out_status: ?*sec.Status) u16 {
    var local: sec.Status = .{};
    if (ra8_sram_get_status(&local) != k_ra8_ok) return 0;
    if (out_status) |out| out.* = local;
    return sec.dispatchEsr(&local, Dispatcher{});
}
