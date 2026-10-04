//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the ra8_dtc_* driver (RA8FW-588). The handler and vector-base
//! state lives here; the sequences are in internal/dtc.zig.

const common = @import("abi_common.zig");
const dtc = @import("internal/dtc.zig");

const tag = "DTC";

extern fn ra8_mstp_enable(id: u16) u16;
extern fn ra8_mstp_disable(id: u16) u16;
extern fn ra8_cache_dcache_clean_by_addr(addr: *const anyopaque, size: u32) u16;
extern fn ra8_isr_set_dtc(slot: u16, enable: bool) u16;

/// `ra8_dtc_ti_t`: the TI block at its 16-byte alignment.
const TiSlot = extern struct { ti: dtc.Ti align(dtc.ti_align) };

comptime {
    // The static_asserts from ra8_dtc.c (HUM Figure 18.4, Ch 18.3.1, 18.2.2).
    if (@sizeOf(dtc.Ti) != dtc.ti_size) @compileError("TI block must be 16 bytes");
    if (@alignOf(TiSlot) != dtc.ti_align) @compileError("TI block must be 16-byte aligned");
    if (@offsetOf(dtc.Ti, "cra") != 0x0E) @compileError("CRA must sit at +0x0E");
}

var state: dtc.State = .{};

const Mmio = struct {
    fn ptr(comptime T: type, off: usize) *volatile T {
        return @ptrFromInt(dtc.base_addr + off);
    }
    pub fn read16(_: Mmio, off: usize) u16 {
        return ptr(u16, off).*;
    }
    pub fn write8(_: Mmio, off: usize, v: u8) void {
        ptr(u8, off).* = v;
    }
    pub fn write16(_: Mmio, off: usize, v: u16) void {
        ptr(u16, off).* = v;
    }
    pub fn write32(_: Mmio, off: usize, v: u32) void {
        ptr(u32, off).* = v;
    }
};

const C = struct {
    pub fn mstpEnable(_: C, id: u16) u16 {
        return ra8_mstp_enable(id);
    }
    pub fn mstpDisable(_: C, id: u16) u16 {
        return ra8_mstp_disable(id);
    }
    pub fn cacheClean(_: C, addr: *const anyopaque, size: u32) u16 {
        return ra8_cache_dcache_clean_by_addr(addr, size);
    }
    pub fn isrSetDtc(_: C, slot: u16, enable: bool) u16 {
        return ra8_isr_set_dtc(slot, enable);
    }
    pub fn nullPtr(_: C, msg: [*:0]const u8) u16 {
        common.ra8_log_emit_error(tag, msg);
        return common.k_ra8_err_null_ptr;
    }
    pub fn logError(_: C, msg: [*:0]const u8) void {
        common.ra8_log_emit_error(tag, msg);
    }
    pub fn logInfo(_: C, msg: [*:0]const u8) void {
        common.ra8_log_emit_info(tag, msg);
    }
    pub fn fail(_: C, msg: [*:0]const u8, err: u16) void {
        common.ra8_log_emit_error(tag, msg);
        common.ra8_log_emit_error_val(tag, "Error", err);
    }
};

export fn ra8_dtc_init(base: ?*anyopaque) u16 {
    return state.init(Mmio{}, C{}, base);
}

export fn ra8_dtc_deinit() u16 {
    return state.deinit(Mmio{}, C{});
}

export fn ra8_dtc_enable() u16 {
    return state.enable(Mmio{}, C{});
}

export fn ra8_dtc_disable() u16 {
    return dtc.disable(Mmio{});
}

export fn ra8_dtc_reconfigure(base: ?*anyopaque) u16 {
    return state.reconfigure(Mmio{}, C{}, base);
}

export fn ra8_dtc_get_status(out: ?*u16) u16 {
    const o = out orelse return C.nullPtr(C{}, "out_mask must not be nullptr");
    o.* = dtc.status(Mmio{});
    return common.k_ra8_ok;
}

export fn ra8_dtc_clear_status(mask: u16) u16 {
    return dtc.clearStatus(Mmio{}, mask);
}

export fn ra8_dtc_attach_handler(f: ?dtc.EventFn, ctx: ?*anyopaque) u16 {
    return state.attach(f, ctx);
}

export fn ra8_dtc_dispatch() void {
    state.dispatch(Mmio{});
}

export fn ra8_dtc_describe(cfg: ?*const dtc.Cfg, ti: ?*TiSlot) u16 {
    return dtc.describe(C{}, cfg, if (ti) |t| &t.ti else null);
}

export fn ra8_dtc_bind_activation(slot: u16, cfg: ?*const dtc.Cfg, ti: ?*TiSlot) u16 {
    return state.bind(C{}, slot, cfg, if (ti) |t| &t.ti else null);
}

export fn ra8_dtc_enter_stop() u16 {
    return dtc.enterStop(Mmio{}, C{});
}

export fn ra8_dtc_exit_stop() u16 {
    return dtc.exitStop(C{});
}
