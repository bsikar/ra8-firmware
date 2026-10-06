//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! txm_helium_m85's module (RA8FW-820, under RA8FW-428): a ThreadX module
//! built for the M85 with MVE whose start thread keeps its own Helium state.
//!
//! It loads S0-S31 (Q0-Q7) with `lane_base + n` and VPR.P0 with `vpr_p0`,
//! then spins checking every lane and P0 without ever calling the kernel.
//! It never yields, so each time it runs again it was preempted (SysTick, a
//! higher priority kernel thread waking) and resumed by the scheduler's
//! exception return: the path whose save and restore RA8FW-428 asks about.
//! Each resume bumps the start thread's run count, which the manager reads.
//!
//! On the first wrong lane or P0 it leaves the loop and sleeps forever, so
//! its run count stops rising: that is the failure the manager sees.
//! Everything is in one asm block, so no compiled code touches the
//! registers between loading and checking them.

const std = @import("std");

/// S0's value; S{n} holds lane_base + n. The kernel thread uses another.
pub const lane_base: u32 = 0x4D50_0000;
/// VPR.P0 while the module runs.
pub const vpr_p0: u32 = 0x5A5A;
/// S0-S31, the eight Q registers' lanes.
pub const lanes = 32;

/// TX_WAIT_FOREVER.
const forever: u32 = 0xFFFF_FFFF;

extern fn _tx_thread_sleep(timer_ticks: u32) u32;

/// `r1 = lane_base`.
const set_base = std.fmt.comptimePrint(
    "movw r1, #{d}\nmovt r1, #{d}\n",
    .{ lane_base & 0xFFFF, lane_base >> 16 },
);

/// S{n} = r1 + n, for every lane.
const load_lanes = blk: {
    var text: []const u8 = set_base;
    for (0..lanes) |n| text = text ++ std.fmt.comptimePrint("vmov s{d}, r1\nadds r1, r1, #1\n", .{n});
    break :blk text;
};

/// Branch to 2 on the first lane that is not r1 + n.
const check_lanes = blk: {
    var text: []const u8 = set_base;
    for (0..lanes) |n| text = text ++ std.fmt.comptimePrint("vmov r2, s{d}\ncmp r2, r1\nbne 2f\nadds r1, r1, #1\n", .{n});
    break :blk text;
};

const spin = load_lanes ++ std.fmt.comptimePrint("movw r3, #{d}\nvmsr p0, r3\n", .{vpr_p0}) ++
    "1:\nvmrs r2, p0\ncmp r2, r3\nbne 2f\n" ++ check_lanes ++ "b 1b\n2:\n";

/// The module's start thread, entered with the module's ID.
export fn demo_module_start(id: u32) callconv(.c) noreturn {
    _ = id;
    // Only the core registers are named: this thread never returns and
    // holds no FP value of its own, so S0-S31 being taken over is nothing
    // the compiler has to preserve (and naming all 32 is past Zig's limit).
    asm volatile (spin ::: "r1", "r2", "r3", "cc", "memory");
    while (true) _ = _tx_thread_sleep(forever);
}
