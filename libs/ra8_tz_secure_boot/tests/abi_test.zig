//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The membrane, exercised through the exact signatures C calls it by.
//!
//! These are the rules a host test can hold that the decision-core tests
//! cannot: the order of the register stores, that the protection gate closes
//! on every path including the failing ones, and that a refused call returns
//! before anything moves.

const std = @import("std");
const abi = @import("abi");
const boot = abi.boot;
const hal = abi.hal;
const ipc = abi.ipc;
const partition = abi.partition;
const regs = abi.regs;
const sau = abi.sau;

/// A 4-byte-aligned NS vector table: initial SP, then the reset entry.
fn nsTable(sp: u32, entry: u32) [2]u32 {
    return .{ sp, entry };
}

fn fresh() void {
    abi.ra8_tz_secure_boot_host_reset();
}

test "sau_init refuses silicon short of the five published regions" {
    fresh();
    hal.fake.implemented = 4;
    try std.testing.expectEqual(regs.Err.not_supported, abi.ra8_tz_secure_boot_sau_init());
    // Refused before the driver was called at all.
    try std.testing.expectEqual(@as(u8, 0), hal.fake.configure_count);
    try std.testing.expectEqual(boot.Step.idle, boot.step);
}

test "sau_init hands the driver the five-region partition and stamps the step" {
    fresh();
    hal.fake.implemented = 8;
    try std.testing.expectEqual(regs.Err.ok, abi.ra8_tz_secure_boot_sau_init());
    try std.testing.expectEqual(@as(u8, 1), hal.fake.configure_count);
    try std.testing.expectEqual(@as(u8, sau.Region.count), hal.fake.last_region_count);
    try std.testing.expect(!hal.fake.last_all_ns);
    try std.testing.expectEqual(boot.Step.sau_done, boot.step);
}

test "a driver refusal is passed through and does not claim the step" {
    fresh();
    hal.fake.configure_result = regs.Err.invalid_arg;
    try std.testing.expectEqual(regs.Err.invalid_arg, abi.ra8_tz_secure_boot_sau_init());
    try std.testing.expectEqual(boot.Step.idle, boot.step);
}

test "security_init writes both words inside a balanced gate" {
    fresh();
    try std.testing.expectEqual(regs.Err.ok, abi.ra8_tz_secure_boot_security_init(0x50000, 0x3));
    try std.testing.expectEqual(@as(u32, 0x50000), boot.host.ipcsar_value);
    try std.testing.expectEqual(@as(u32, 0x3), boot.host.ipcpar_value);
    try std.testing.expectEqual(@as(u8, 1), boot.host.prcr_unlock_count);
    try std.testing.expect(boot.host.balanced());
    // The gate is left closed, not merely balanced.
    try std.testing.expectEqual(regs.Prcr.close, boot.host.prcr_s_last);
    try std.testing.expectEqual(boot.Step.prcr_relocked, boot.step);
}

test "security_init_map refuses a bad descriptor before the gate opens" {
    fresh();
    var cfg = ipc.cpu1Pingpong();
    cfg.target[0].world = @enumFromInt(7);
    try std.testing.expectEqual(
        regs.Err.invalid_arg,
        abi.ra8_tz_secure_boot_security_init_map(&cfg),
    );
    try std.testing.expectEqual(@as(u8, 0), boot.host.prcr_unlock_count);
    try std.testing.expectEqual(@as(u32, 0), boot.host.ipcsar_value);
}

test "security_init_map applies the cpu1_pingpong map" {
    fresh();
    const cfg = ipc.cpu1Pingpong();
    try std.testing.expectEqual(regs.Err.ok, abi.ra8_tz_secure_boot_security_init_map(&cfg));
    try std.testing.expectEqual(@as(u32, 0x50000), boot.host.ipcsar_value);
    try std.testing.expect(boot.host.balanced());
}

test "every pointer the C ABI may pass as NULL is refused, not dereferenced" {
    fresh();
    try std.testing.expectEqual(regs.Err.null_ptr, abi.ra8_tz_secure_boot_security_init_map(null));
    try std.testing.expectEqual(regs.Err.null_ptr, abi.ra8_tz_secure_boot_jump_ns(null));
    try std.testing.expectEqual(regs.Err.null_ptr, abi.ra8_tz_secure_boot_run(0, 0, null));
    try std.testing.expectEqual(regs.Err.null_ptr, abi.ra8_tz_partition_validate(null));
    try std.testing.expectEqual(regs.Err.null_ptr, abi.ra8_tz_partition_apply(null));
    try std.testing.expectEqual(regs.Err.null_ptr, abi.ra8_tz_ipc_cpu1_pingpong(null));
    try std.testing.expectEqual(@as(u32, 0), abi.ra8_tz_ns_signed_body_len(null));
}

test "encode refuses on a null output without writing the other one" {
    fresh();
    const cfg = ipc.cpu1Pingpong();
    var sar: u32 = 0xAAAA;
    try std.testing.expectEqual(
        regs.Err.null_ptr,
        abi.ra8_tz_ipc_attribution_encode(&cfg, &sar, null),
    );
    try std.testing.expectEqual(@as(u32, 0xAAAA), sar);
}

test "encode writes both words for a good descriptor" {
    fresh();
    const cfg = ipc.cpu1Pingpong();
    var sar: u32 = 0;
    var par: u32 = 0;
    try std.testing.expectEqual(
        regs.Err.ok,
        abi.ra8_tz_ipc_attribution_encode(&cfg, &sar, &par),
    );
    try std.testing.expectEqual(@as(u32, 0x50000), sar);
    try std.testing.expectEqual(@as(u32, 0), par);
}

test "jump_ns rejects the two vectors that mean no image" {
    fresh();
    const zeroed = nsTable(0x2000_0000, 0);
    try std.testing.expectEqual(regs.Err.invalid_arg, abi.ra8_tz_secure_boot_jump_ns(&zeroed));
    const erased = nsTable(0x2000_0000, 0xFFFF_FFFF);
    try std.testing.expectEqual(regs.Err.invalid_arg, abi.ra8_tz_secure_boot_jump_ns(&erased));
    try std.testing.expectEqual(@as(u32, 0), boot.host.vtor_ns);
}

test "jump_ns arms VTOR and captures what it would have branched to" {
    fresh();
    const table = nsTable(0x2010_0000, 0x0208_0401);
    try std.testing.expectEqual(regs.Err.ok, abi.ra8_tz_secure_boot_jump_ns(&table));
    try std.testing.expectEqual(@as(u32, @truncate(@intFromPtr(&table))), boot.host.vtor_ns);
    try std.testing.expectEqual(@as(u32, 0x0208_0401), abi.ra8_tz_secure_boot_host_blxns_target());
    try std.testing.expectEqual(@as(u32, 0x2010_0000), boot.host.blxns_msp_ns);
    try std.testing.expectEqual(@as(u8, @intFromEnum(boot.Step.branched)), abi.ra8_tz_secure_boot_get_step());
}

test "run walks sau, security and jump in that order" {
    fresh();
    const table = nsTable(0x2010_0000, 0x0208_0401);
    try std.testing.expectEqual(regs.Err.ok, abi.ra8_tz_secure_boot_run(0x50000, 0, &table));
    try std.testing.expectEqual(@as(u8, 1), hal.fake.configure_count);
    try std.testing.expectEqual(@as(u32, 0x50000), boot.host.ipcsar_value);
    try std.testing.expectEqual(@as(u32, 0x0208_0401), boot.host.blxns_target);
    try std.testing.expect(boot.host.balanced());
}

test "run stops at the first failure and never reaches the branch" {
    fresh();
    hal.fake.implemented = 4;
    const table = nsTable(0x2010_0000, 0x0208_0401);
    try std.testing.expectEqual(
        regs.Err.not_supported,
        abi.ra8_tz_secure_boot_run(0x50000, 0, &table),
    );
    try std.testing.expectEqual(@as(u32, 0), boot.host.blxns_target);
    try std.testing.expectEqual(@as(u32, 0), boot.host.ipcsar_value);
}

test "the board map the ABI hands out is the one the validator accepts" {
    fresh();
    const map = abi.ra8_tz_partition_board_map();
    try std.testing.expectEqual(@as(u8, 4), map.sau_region_count);
    try std.testing.expectEqual(regs.Err.ok, abi.ra8_tz_partition_validate(map));
}

test "apply programs the SAU and leaves the boundary alone when none is named" {
    fresh();
    try std.testing.expectEqual(regs.Err.ok, abi.ra8_tz_partition_apply(&partition.board_map));
    try std.testing.expectEqual(@as(u8, 1), hal.fake.configure_count);
    try std.testing.expectEqual(@as(u8, 0), hal.fake.boundary_count);
    // No boundary work means the gate was never opened.
    try std.testing.expectEqual(@as(u8, 0), boot.host.prcr_unlock_count);
}

test "apply writes every bank behind a gate it closes again" {
    fresh();
    const boundary = [_]u32{ 0, 0x2000, 0x4000, 0x6000 };
    var map = partition.board_map;
    map.sram_boundary = &boundary;
    try std.testing.expectEqual(regs.Err.ok, abi.ra8_tz_partition_apply(&map));
    try std.testing.expectEqual(@as(u8, 4), hal.fake.boundary_count);
    try std.testing.expectEqual(@as(u32, 0x6000), hal.fake.last_offset);
    try std.testing.expect(boot.host.balanced());
    try std.testing.expectEqual(regs.Prcr.close, boot.host.prcr_s_last);
}

test "a bank that refuses stops the run and STILL closes the gate" {
    fresh();
    const boundary = [_]u32{ 0, 0x2000, 0x4000, 0x6000 };
    var map = partition.board_map;
    map.sram_boundary = &boundary;
    hal.fake.boundary_result = regs.Err.invalid_arg;
    try std.testing.expectEqual(regs.Err.invalid_arg, abi.ra8_tz_partition_apply(&map));
    try std.testing.expectEqual(@as(u8, 1), hal.fake.boundary_count);
    // This is the rule the C macro enforced with a loop increment: leaving
    // the protected scope early must not skip the re-lock.
    try std.testing.expect(boot.host.balanced());
    try std.testing.expectEqual(regs.Prcr.close, boot.host.prcr_s_last);
}

test "apply refuses an invalid descriptor before the driver is called" {
    fresh();
    var map = partition.board_map;
    map.sau_region_count = 99;
    try std.testing.expectEqual(regs.Err.not_supported, abi.ra8_tz_partition_apply(&map));
    try std.testing.expectEqual(@as(u8, 0), hal.fake.configure_count);
}

test "psar refuses a null register address before the gate opens" {
    fresh();
    var seen: u32 = 0;
    try std.testing.expectEqual(regs.Err.invalid_arg, abi.ra8_tz_psar_set_ns(0, 0xF, &seen));
    try std.testing.expectEqual(@as(u8, 0), boot.host.prcr_unlock_count);
}

test "an empty mask observes without opening the gate" {
    fresh();
    boot.host.write32(0x4000_8000, 0x30);
    var seen: u32 = 0;
    try std.testing.expectEqual(regs.Err.ok, abi.ra8_tz_psar_set_ns(0x4000_8000, 0, &seen));
    try std.testing.expectEqual(@as(u32, 0x30), seen);
    try std.testing.expectEqual(@as(u8, 0), boot.host.prcr_unlock_count);
}

test "psar ORs the mask in, reports what stuck and closes the gate" {
    fresh();
    boot.host.write32(0x4000_8000, 0b0100);
    var seen: u32 = 0;
    try std.testing.expectEqual(regs.Err.ok, abi.ra8_tz_psar_set_ns(0x4000_8000, 0b0011, &seen));
    // Bits already handed over stay handed over.
    try std.testing.expectEqual(@as(u32, 0b0111), seen);
    try std.testing.expect(boot.host.balanced());
    try std.testing.expectEqual(regs.Prcr.close, boot.host.prcr_s_last);
}

test "psar tolerates a caller that does not want the read-back" {
    fresh();
    try std.testing.expectEqual(regs.Err.ok, abi.ra8_tz_psar_set_ns(0x4000_8000, 0b1, null));
    try std.testing.expect(boot.host.balanced());
}

test "get_step tracks the sequence rather than being written by hand" {
    fresh();
    try std.testing.expectEqual(@as(u8, 0), abi.ra8_tz_secure_boot_get_step());
    _ = abi.ra8_tz_secure_boot_sau_init();
    try std.testing.expectEqual(@as(u8, 1), abi.ra8_tz_secure_boot_get_step());
    _ = abi.ra8_tz_secure_boot_security_init(0, 0);
    try std.testing.expectEqual(@as(u8, 4), abi.ra8_tz_secure_boot_get_step());
}

test "host_reset returns both the sequencer and the driver fake to power-on" {
    fresh();
    _ = abi.ra8_tz_secure_boot_sau_init();
    _ = abi.ra8_tz_secure_boot_security_init(0xDEAD, 0xBEEF);
    fresh();
    try std.testing.expectEqual(@as(u8, 0), abi.ra8_tz_secure_boot_get_step());
    try std.testing.expectEqual(@as(u32, 0), boot.host.ipcsar_value);
    try std.testing.expectEqual(@as(u8, 0), hal.fake.configure_count);
}
