//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for ra8_rmac_get_status, ra8_rmac_clear_status and
//! ra8_rmac_read_stats (RA8FW-748). Bases are k_ra8_rmac0/1_base_addr.

const common = @import("abi_common.zig");
const mg = @import("internal/rmac_mgmt.zig");

const tag = "RMAC";
const port_bases = [_]usize{ 0x403C_B000, 0x403C_D000 };

const Mmio = struct {
    base: usize,
    pub fn read32(self: Mmio, o: usize) u32 {
        const p: *volatile u32 = @ptrFromInt(self.base + o);
        return p.*;
    }
    pub fn write32(self: Mmio, o: usize, v: u32) void {
        const p: *volatile u32 = @ptrFromInt(self.base + o);
        p.* = v;
    }
};

const C = struct {
    pub fn logError(_: C, msg: [*:0]const u8) void {
        common.ra8_log_emit_error(tag, msg);
    }
};

fn mmioFor(port: u8) Mmio {
    return .{ .base = port_bases[if (port == 1) 1 else 0] };
}

export fn ra8_rmac_get_status(port: u8, out: ?*mg.Status) u16 {
    return mg.getStatus(mmioFor(port), C{}, port, out);
}

export fn ra8_rmac_clear_status(port: u8, err_mask: u32, mon0: u32, mon1: u32, mon2: u32) u16 {
    return mg.clearStatus(mmioFor(port), C{}, port, err_mask, .{ mon0, mon1, mon2 });
}

export fn ra8_rmac_read_stats(port: u8, out: ?*mg.Stats) u16 {
    return mg.readStats(mmioFor(port), C{}, port, out);
}
