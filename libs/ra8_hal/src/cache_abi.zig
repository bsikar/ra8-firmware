//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for ra8_cache.h (RA8FW-612). Barriers are real on Arm targets
//! and no-ops on the host.

const builtin = @import("builtin");
const common = @import("abi_common.zig");
const cache = @import("internal/cache.zig");

const tag = "ra8_cache";
const is_arm = builtin.cpu.arch.isArm() or builtin.cpu.arch.isThumb();

const Hw = struct {
    pub fn read32(_: Hw, addr: usize) u32 {
        return @as(*volatile u32, @ptrFromInt(addr)).*;
    }
    pub fn write32(_: Hw, addr: usize, value: u32) void {
        @as(*volatile u32, @ptrFromInt(addr)).* = value;
    }
    pub fn dsb(_: Hw) void {
        if (comptime is_arm) asm volatile ("dsb 0xF" ::: "memory");
    }
    pub fn isb(_: Hw) void {
        if (comptime is_arm) asm volatile ("isb 0xF" ::: "memory");
    }
    pub fn err(_: Hw, msg: [*:0]const u8) void {
        common.ra8_log_emit_error(tag, msg);
    }
};

fn range(addr: ?*const anyopaque, size: u32, reg: usize) u16 {
    return cache.maintainRange(Hw{}, @intFromPtr(addr), size, reg);
}

export fn ra8_cache_dcache_line_bytes() u32 {
    return cache.lineBytes(Hw{});
}

export fn ra8_cache_dcache_clean_by_addr(addr: ?*const anyopaque, size: u32) u16 {
    return range(addr, size, cache.dccmvac);
}

export fn ra8_cache_dcache_invalidate_by_addr(addr: ?*const anyopaque, size: u32) u16 {
    return range(addr, size, cache.dcimvac);
}

export fn ra8_cache_dcache_clean_invalidate_by_addr(addr: ?*const anyopaque, size: u32) u16 {
    return range(addr, size, cache.dccimvac);
}

export fn ra8_cache_dcache_invalidate_all() void {
    cache.setwayAll(Hw{}, cache.dcisw);
}

export fn ra8_cache_icache_invalidate_all() void {
    cache.icacheInvalidateAll(Hw{});
}

export fn ra8_cache_icache_enable() void {
    cache.icacheEnable(Hw{});
}

export fn ra8_cache_icache_disable() void {
    cache.icacheDisable(Hw{});
}

export fn ra8_cache_dcache_enable() void {
    cache.dcacheEnable(Hw{});
}

export fn ra8_cache_dcache_disable() void {
    cache.dcacheDisable(Hw{});
}

export fn ra8_cache_enable() void {
    cache.icacheEnable(Hw{});
    cache.dcacheEnable(Hw{});
}
