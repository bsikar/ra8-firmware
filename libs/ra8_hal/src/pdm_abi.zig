//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the ra8_pdm_* driver (RA8FW-595). The per-channel stream state
//! lives here; the register sequences are in internal/pdm.zig.

const common = @import("abi_common.zig");
const pdm = @import("internal/pdm.zig");

const tag = "PDM";

const IsrFn = *const fn (ctx: ?*anyopaque) callconv(.C) void;

extern fn ra8_mstp_enable(id: u16) u16;
extern fn ra8_mstp_disable(id: u16) u16;
extern fn ra8_isr_register(event: u16, handler: IsrFn, ctx: ?*anyopaque, priority: u8, out_slot: ?*u16) u16;
extern fn ra8_isr_unregister(event: u16) u16;

var streams = [_]pdm.Stream{.{}} ** pdm.ch_count;

const Mmio = struct {
    pub fn read32(_: Mmio, off: usize) u32 {
        return @as(*volatile u32, @ptrFromInt(pdm.base + off)).*;
    }
    pub fn write32(_: Mmio, off: usize, value: u32) void {
        @as(*volatile u32, @ptrFromInt(pdm.base + off)).* = value;
    }
};

fn dataIsr(ctx: ?*anyopaque) callconv(.C) void {
    const ch: u8 = @truncate(@intFromPtr(ctx));
    pdm.dataIsr(Mmio{}, &streams, ch);
}

const C = struct {
    pub fn mstpEnable(_: C, id: u16) u16 {
        return ra8_mstp_enable(id);
    }
    pub fn mstpDisable(_: C, id: u16) u16 {
        return ra8_mstp_disable(id);
    }
    pub fn isrRegister(_: C, event: u16, ch: u8, priority: u8) u16 {
        return ra8_isr_register(event, dataIsr, @ptrFromInt(ch), priority, null);
    }
    pub fn isrUnregister(_: C, event: u16) u16 {
        return ra8_isr_unregister(event);
    }
    pub fn info(_: C, msg: [*:0]const u8) void {
        common.ra8_log_emit_info(tag, msg);
    }
    pub fn err(_: C, msg: [*:0]const u8) void {
        common.ra8_log_emit_error(tag, msg);
    }
    pub fn fail(_: C, msg: [*:0]const u8, code: u16) void {
        common.ra8_log_emit_error(tag, msg);
        common.ra8_log_emit_error_val(tag, "Error", code);
    }
};

export fn ra8_pdm_init() u16 {
    return pdm.init(Mmio{}, C{});
}

export fn ra8_pdm_deinit() u16 {
    return pdm.deinit(Mmio{}, C{});
}

export fn ra8_pdm_configure(ch: u8, cfg: ?*const pdm.Config) u16 {
    return pdm.configure(Mmio{}, C{}, ch, cfg);
}

export fn ra8_pdm_start(ch: u8) u16 {
    return pdm.start(Mmio{}, C{}, ch);
}

export fn ra8_pdm_read_enable(ch: u8) u16 {
    return pdm.readEnable(Mmio{}, C{}, ch);
}

export fn ra8_pdm_read(ch: u8, out: ?[*]i32, max: u32, out_count: ?*u32) u16 {
    return pdm.read(Mmio{}, C{}, ch, out, max, out_count);
}

export fn ra8_pdm_stream_enable(ch: u8, cb: ?pdm.DataFn, ctx: ?*anyopaque, priority: u8) u16 {
    return pdm.streamEnable(Mmio{}, C{}, &streams, ch, cb, ctx, priority);
}

export fn ra8_pdm_stream_disable(ch: u8) u16 {
    return pdm.streamDisable(Mmio{}, C{}, &streams, ch);
}

export fn ra8_pdm_stop(ch: u8) u16 {
    return pdm.stop(Mmio{}, C{}, &streams, ch);
}
