//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for ra8_reset.h (RA8FW-752), replacing ra8_reset.c. The logic is in
//! internal/reset.zig; this file owns the boot-cause cache and the volatile
//! access at SYSC 0x4001E000 and SCB AIRCR 0xE000ED0C.

const builtin = @import("builtin");
const common = @import("abi_common.zig");
const rs = @import("internal/reset.zig");

const tag = "RESET";
const ok = rs.codes.ok;

const Hw = struct {
    fn reg(comptime T: type, o: usize) *volatile T {
        return @ptrFromInt(rs.sysc_base + o);
    }
    pub fn read8(_: Hw, o: usize) u8 {
        return reg(u8, o).*;
    }
    pub fn write8(_: Hw, o: usize, v: u8) void {
        reg(u8, o).* = v;
    }
    pub fn read16(_: Hw, o: usize) u16 {
        return reg(u16, o).*;
    }
    pub fn write16(_: Hw, o: usize, v: u16) void {
        reg(u16, o).* = v;
    }
    pub fn read32(_: Hw, o: usize) u32 {
        return reg(u32, o).*;
    }
    pub fn write32(_: Hw, o: usize, v: u32) void {
        reg(u32, o).* = v;
    }
};

var initialized: bool = false;
var cached_cause: u8 = rs.cause.unknown;
var cached_raw: rs.Raw = .{};

fn nullOut(msg: [*:0]const u8) u16 {
    common.ra8_log_emit_error(tag, msg);
    return rs.codes.null_ptr;
}

fn refresh() void {
    cached_raw = rs.readRaw(Hw{});
    cached_cause = rs.decode(cached_raw);
}

export fn ra8_reset_init() u16 {
    refresh();
    initialized = true;
    common.ra8_log_emit_info_val(tag, "boot cause", cached_cause);
    return ok;
}

export fn ra8_reset_test_only_reset_state() void {
    initialized = false;
    cached_cause = rs.cause.unknown;
    cached_raw = .{};
}

export fn ra8_reset_get_cause(out: ?*u8) u16 {
    const o = out orelse return nullOut("out must not be nullptr");
    o.* = if (initialized) cached_cause else rs.decode(rs.readRaw(Hw{}));
    return ok;
}

export fn ra8_reset_get_raw(out: ?*rs.Raw) u16 {
    const o = out orelse return nullOut("out must not be nullptr");
    o.* = if (initialized) cached_raw else rs.readRaw(Hw{});
    return ok;
}

export fn ra8_reset_clear_cause(mask: u32) u16 {
    rs.clear(Hw{}, mask);
    if (initialized) refresh();
    return ok;
}

export fn ra8_reset_get_attribution(out: ?*u32) u16 {
    const o = out orelse return nullOut("out must not be nullptr");
    const hw = Hw{};
    o.* = hw.read32(rs.off.rstsar);
    return ok;
}

const on_target = builtin.os.tag == .freestanding;
const host = struct {
    extern fn ra8_hw_wait_for_reset() void;
};

export fn ra8_reset_software_reset() void {
    common.ra8_log_emit_info(tag, "software reset");
    @as(*volatile u32, @ptrFromInt(rs.aircr_addr)).* = rs.aircr_reset;
    if (comptime on_target) {
        asm volatile ("dsb 0xF" ::: .{ .memory = true });
        while (true) {}
    } else host.ra8_hw_wait_for_reset();
}

export fn ra8_reset_set_source_mask(source: u8, disable: bool) u16 {
    const loc = rs.sourceLoc(source) orelse {
        common.ra8_log_emit_error(tag, "set_source_mask: invalid reset source");
        return rs.codes.invalid_arg;
    };
    rs.setSourceMask(Hw{}, loc, disable);
    return ok;
}

export fn ra8_reset_get_source_mask(source: u8, disabled: ?*bool) u16 {
    const d = disabled orelse return nullOut("disabled must not be nullptr");
    const loc = rs.sourceLoc(source) orelse {
        common.ra8_log_emit_error(tag, "get_source_mask: invalid reset source");
        return rs.codes.invalid_arg;
    };
    d.* = rs.sourceMasked(Hw{}, loc);
    return ok;
}
