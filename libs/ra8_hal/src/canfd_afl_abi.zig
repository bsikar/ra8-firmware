//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for ra8_canfd_set_accept_filter (RA8FW-583).

const common = @import("abi_common.zig");
const afl = @import("internal/canfd_afl.zig");
const timing = @import("internal/canfd_timing.zig");

const tag = "CANFD";
const null_ptr: u16 = 0x504;

/// Volatile word access at a CANFD channel base.
const Mmio = struct {
    base: usize,

    pub fn read(self: Mmio, offset: usize) u32 {
        const p: *volatile u32 = @ptrFromInt(self.base + offset);
        return p.*;
    }
    pub fn write(self: Mmio, offset: usize, value: u32) void {
        const p: *volatile u32 = @ptrFromInt(self.base + offset);
        p.* = value;
    }
};

export fn ra8_canfd_set_accept_filter(channel: u8, rules: ?[*]const afl.Rule, count: u8) u16 {
    const list = rules orelse {
        common.ra8_log_emit_error(tag, "rules must not be nullptr");
        return null_ptr;
    };
    const base = timing.channelBase(channel) orelse {
        common.ra8_log_emit_error(tag, "channel out of range");
        return null_ptr;
    };
    if (count == 0 or count > afl.rule_capacity) return afl.invalid_arg;
    const slice = list[0..count];
    const v = afl.validate(slice);
    if (v != afl.ok) return v;
    afl.program(Mmio{ .base = base }, slice);
    return afl.ok;
}
