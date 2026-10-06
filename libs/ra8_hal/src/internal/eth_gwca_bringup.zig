//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! GWCA LINKFIX install and the bring-up sequence (RA8FW-850, was part of
//! ra8_eth_gwca.c). Exports and the J-Link step globals live in
//! src/eth_gwca_bringup_abi.zig.

pub const q = @import("eth_gwca_queue.zig");

pub const ok: u16 = 0;
pub const invalid_arg: u16 = 0x103;
pub const null_ptr: u16 = 0x504;

/// LINKFIX table limit, one entry per queue.
pub const linkfix_max_entries: u32 = 32;
/// `k_ra8_gwdcc_dt_lempty`: queue disabled.
pub const dt_lempty: u8 = 12;

/// `ra8_gwmc_opc_t`.
pub const opc_disable: u32 = 1;
pub const opc_config: u32 = 2;
pub const opc_operation: u32 = 3;

/// `k_ra8_eth_gwca_step_ok_N` is N; `step_fail_N` is 0x10 + N.
pub fn stepOk(n: u32) u32 {
    return n;
}
pub fn stepFail(n: u32) u32 {
    return 0x10 + n;
}

/// Mark every entry LEMPTY, then point GWDCBAC0/1 at the table.
/// `hw` supplies logError.
pub fn installLinkfix(hw: anytype, table: ?[*]volatile q.Desc, count: u32, gwdcbac0: *volatile u32, gwdcbac1: *volatile u32) u16 {
    const t = table orelse {
        hw.logError("install_linkfix: table must not be null");
        return null_ptr;
    };
    if (count == 0 or count > linkfix_max_entries) {
        hw.logError("install_linkfix: entry_count out of range");
        return invalid_arg;
    }
    for (t[0..count]) |*d| {
        d.* = .{};
        q.setDt(d, dt_lempty);
    }
    const addr: u64 = @intFromPtr(t);
    gwdcbac0.* = @truncate((addr >> 32) & 0xFF);
    gwdcbac1.* = @truncate(addr & 0xFFFF_FFFF);
    return ok;
}

/// DISABLE, CONFIG, AXI init, LINKFIX. On a failure past the first step the
/// block is dropped back to DISABLE. `ops` supplies setMode, axiInit,
/// installLinkfix and step.
fn toConfig(ops: anytype, table: ?[*]volatile q.Desc, count: u32) u16 {
    var err = ops.setMode(opc_disable);
    if (err != ok) {
        ops.step(stepFail(1));
        return err;
    }
    ops.step(stepOk(1));
    err = ops.setMode(opc_config);
    if (err == ok) {
        ops.step(stepOk(2));
        err = ops.axiInit();
        if (err == ok) {
            ops.step(stepOk(3));
            err = ops.installLinkfix(table, count);
            if (err == ok) {
                ops.step(stepOk(4));
                return ok;
            }
            ops.step(stepFail(4));
        } else ops.step(stepFail(3));
    } else ops.step(stepFail(2));
    _ = ops.setMode(opc_disable);
    return err;
}

/// Full bring-up: config phase, then DISABLE and OPERATION.
pub fn bringUp(ops: anytype, table: ?[*]volatile q.Desc, count: u32) u16 {
    ops.step(0);
    const phase1 = toConfig(ops, table, count);
    if (phase1 != ok) return phase1;
    const dis = ops.setMode(opc_disable);
    if (dis != ok) {
        ops.step(stepFail(5));
        return dis;
    }
    ops.step(stepOk(5));
    const op = ops.setMode(opc_operation);
    if (op != ok) {
        ops.step(stepFail(6));
        return op;
    }
    ops.step(stepOk(6));
    return ok;
}
