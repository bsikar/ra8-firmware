//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the ETHA TAS entry points (RA8FW-590). Validates the port and
//! builds that port's register view; the flows are in internal/etha_tas.zig.

const common = @import("abi_common.zig");
const tas = @import("internal/etha_tas.zig");

const tag = "ETHA";

const Mmio = struct {
    base: usize,
    pub fn read32(self: Mmio, off: usize) u32 {
        const p: *volatile u32 = @ptrFromInt(self.base + off);
        return p.*;
    }
    pub fn write32(self: Mmio, off: usize, v: u32) void {
        const p: *volatile u32 = @ptrFromInt(self.base + off);
        p.* = v;
    }
};

const C = struct {
    pub fn logError(_: C, msg: [*:0]const u8) void {
        common.ra8_log_emit_error(tag, msg);
    }
};

fn port(p: u8, msg: [*:0]const u8) ?Mmio {
    const base = tas.portBase(p) orelse {
        common.ra8_log_emit_error(tag, msg);
        return null;
    };
    return .{ .base = base };
}

fn nullPtr(msg: [*:0]const u8) u16 {
    common.ra8_log_emit_error(tag, msg);
    return common.k_ra8_err_null_ptr;
}

export fn ra8_etha_tas_ram_reset(p: u8) u16 {
    const r = port(p, "etha_tas_ram_reset: port out of range") orelse return tas.invalid_arg;
    return tas.ramReset(r, C{});
}

export fn ra8_etha_set_tas_schedule(p: u8, queues: ?*const tas.Queues, igs: u8, cycle_ns: u32, start: u64) u16 {
    const q = queues orelse return nullPtr("etha_set_tas_schedule: queues null");
    const r = port(p, "etha_set_tas_schedule: port out of range") orelse return tas.invalid_arg;
    return tas.setSchedule(r, C{}, q, igs, cycle_ns, start);
}

export fn ra8_etha_read_tas_entry(p: u8, address: u8, out: ?*tas.Entry) u16 {
    const o = out orelse return nullPtr("etha_read_tas_entry: out null");
    const r = port(p, "etha_read_tas_entry: port out of range") orelse return tas.invalid_arg;
    return tas.readEntry(r, C{}, address, o);
}

export fn ra8_etha_enable_tas(p: u8, on: u8) u16 {
    const r = port(p, "etha_enable_tas: port out of range") orelse return tas.invalid_arg;
    return tas.enable(r, on != 0);
}
