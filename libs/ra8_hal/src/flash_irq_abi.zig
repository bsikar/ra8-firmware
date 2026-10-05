//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the IRQ, window, erase/write, blank-check and status parts of
//! ra8_flash.h (RA8FW-758), replacing ra8_flash_irq.c. Logic is in
//! internal/flash_irq.zig. ra8_flash.c still owns g_flash_rt, g_flash_tag,
//! the block primitives and the soft-window check.

const builtin = @import("builtin");
const common = @import("abi_common.zig");
const fi = @import("internal/flash_irq.zig");

const ok = common.k_ra8_ok;
const hosted = builtin.os.tag != .freestanding;
const world_ns: u8 = 0;

const Callback = *const fn (ev: *const fi.Event) callconv(.C) void;

/// ra8_flash_runtime_t.
const Runtime = extern struct {
    cb: ?Callback,
    user_ctx: ?*anyopaque,
    initialized: bool,
    prefetch_on: bool,
    win_low: usize,
    win_high: usize,
};

comptime {
    if (@offsetOf(Runtime, "initialized") != 2 * @sizeOf(usize)) @compileError("ra8_flash_runtime_t layout");
    if (@offsetOf(Runtime, "win_low") != 3 * @sizeOf(usize)) @compileError("ra8_flash_runtime_t layout");
}

extern var g_flash_rt: Runtime;
extern var g_flash_tag: [*:0]const u8;
extern fn priv_ra8_flash_internal_window_allows(addr: usize, len: u32) bool;
extern fn ra8_flash_erase_block(mram_addr: u32, world: u8) u16;
extern fn ra8_flash_write_block(mram_addr: u32, data: [*]const u8, len: u32, world: u8) u16;

const Hw = struct {
    fn reg(comptime T: type, o: u16) *volatile T {
        return @ptrFromInt(fi.base + o);
    }
    pub fn read8(_: Hw, o: u16) u8 {
        return reg(u8, o).*;
    }
    pub fn write8(_: Hw, o: u16, v: u8) void {
        reg(u8, o).* = v;
    }
    pub fn read16(_: Hw, o: u16) u16 {
        return reg(u16, o).*;
    }
    pub fn read32(_: Hw, o: u16) u32 {
        return reg(u32, o).*;
    }
};
const hw = Hw{};

const Sink = struct {
    pub fn deliver(_: Sink, s: u8, fault_addr: u32, status_word: u32) void {
        const cb = g_flash_rt.cb orelse return;
        const ev = fi.Event{ .src = s, .fault_addr = fault_addr, .status_word = status_word, .user_ctx = g_flash_rt.user_ctx };
        cb(&ev);
    }
};

fn logError(msg: [*:0]const u8) void {
    common.ra8_log_emit_error(g_flash_tag, msg);
}

/// RA8_RETURN_ON_ERROR: the message, then "Error" with the code.
fn failed(err: u16, msg: [*:0]const u8) bool {
    if (err == ok) return false;
    logError(msg);
    common.ra8_log_emit_error_val(g_flash_tag, "Error", err);
    return true;
}

export fn ra8_flash_set_irq_enable(s: u8, enable: bool) u16 {
    if (s >= fi.src.count) return common.k_ra8_err_invalid_arg;
    return if (fi.setIrq(hw, s, enable)) ok else common.k_ra8_err_invalid_arg;
}

export fn ra8_flash_callback_set(cb: ?Callback, user_ctx: ?*anyopaque) u16 {
    g_flash_rt.cb = cb;
    g_flash_rt.user_ctx = user_ctx;
    return ok;
}

export fn ra8_flash_dispatch_isr() u32 {
    return fi.dispatch(hw, Sink{}, hosted);
}

export fn ra8_flash_set_window(low: usize, high: usize) u16 {
    if (low == 0 and high == 0) {
        g_flash_rt.win_low = 0;
        g_flash_rt.win_high = 0;
        return ok;
    }
    if (low >= high) return common.k_ra8_err_invalid_arg;
    g_flash_rt.win_low = low;
    g_flash_rt.win_high = high;
    return ok;
}

fn validateRange(address: usize, total_len: u64) u16 {
    if (!fi.codeRangeOk(address, total_len)) return common.k_ra8_err_invalid_arg;
    if (!priv_ra8_flash_internal_window_allows(address, @truncate(total_len))) return common.k_ra8_err_out_of_range;
    return ok;
}

export fn ra8_flash_erase(address: usize, num_blocks: u32) u16 {
    if (!g_flash_rt.initialized) {
        logError("flash_erase before init");
        return common.k_ra8_err_not_initialized;
    }
    if (num_blocks == 0) return common.k_ra8_err_invalid_arg;
    const v = validateRange(address, @as(u64, num_blocks) * fi.block_size);
    if (failed(v, "flash_erase: validate")) return v;
    var cur = address;
    for (0..num_blocks) |_| {
        const err = ra8_flash_erase_block(@truncate(cur), world_ns);
        if (err != ok) return err;
        cur += fi.block_size;
    }
    return ok;
}

export fn ra8_flash_write(address: usize, src: ?[*]const u8, len: u32) u16 {
    const data = src orelse {
        logError("src must not be nullptr");
        return common.k_ra8_err_null_ptr;
    };
    if (!g_flash_rt.initialized) {
        logError("flash_write before init");
        return common.k_ra8_err_not_initialized;
    }
    if (len == 0 or len % fi.write_size != 0) return common.k_ra8_err_invalid_arg;
    const v = validateRange(address, len);
    if (failed(v, "flash_write: validate")) return v;
    var i: u32 = 0;
    while (i < len / fi.write_size) : (i += 1) {
        const o = i * fi.write_size;
        const err = ra8_flash_write_block(@truncate(address + o), data + o, fi.write_size, world_ns);
        if (err != ok) return err;
    }
    return ok;
}

export fn ra8_flash_blank_check(address: usize, len: u32, out_blank: ?*bool) u16 {
    const out = out_blank orelse {
        logError("out_blank must not be nullptr");
        return common.k_ra8_err_null_ptr;
    };
    if (len == 0) return common.k_ra8_err_invalid_arg;
    if (!fi.blankRegionOk(address, len)) return common.k_ra8_err_invalid_arg;
    const p: [*]const volatile u8 = @ptrFromInt(address);
    var blank = true;
    for (0..len) |i| {
        if (p[i] != 0xFF) {
            blank = false;
            break;
        }
    }
    out.* = blank;
    return ok;
}

export fn ra8_flash_status(out: ?*fi.Status) u16 {
    const o = out orelse {
        logError("out must not be nullptr");
        return common.k_ra8_err_null_ptr;
    };
    const mrcps = hw.read8(fi.off.mrcps);
    const mastat = hw.read8(fi.off.mastat);
    const mentryr = hw.read16(fi.off.mentryr);
    const mstatr = hw.read32(fi.off.mstatr);
    const prot0 = hw.read16(fi.off.mrcbprot0);
    const prot1 = hw.read16(fi.off.mrcbprot1);
    o.* = fi.decodeStatus(mrcps, mastat, mentryr, mstatr, prot0, prot1);
    return ok;
}
