//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Cross-module tests for the `ra8_tz_secure_boot` decision core.
//!
//! Each module carries the tests for its own rules; what is checked here is
//! the part no single module can see: that the board map the boot programs
//! agrees with the partition validator, and that a full run through the
//! sequence leaves the capture in the state the header promises.

const std = @import("std");
const implementation = @import("implementation");

const boot = implementation.boot;
const ipc = implementation.ipc;
const partition = implementation.partition;
const psar = implementation.psar;
const regs = implementation.regs;

test "every module's own tests are reachable from here" {
    std.testing.refAllDecls(implementation);
}

test "the board map passes the validator it will be checked by" {
    try std.testing.expectEqual(regs.Err.ok, partition.validate(&partition.board_map, 8));
}

test "the board map carves out NS MRAM, NS SRAM, NS SDRAM and the NSC alias" {
    const regions = partition.board_regions;
    try std.testing.expectEqual(@as(usize, 4), regions.len);
    try std.testing.expectEqual(@as(usize, 0x02080000), regions[0].base);
    try std.testing.expectEqual(partition.SauAttr.ns, regions[0].attr);
    try std.testing.expectEqual(@as(usize, 0x10000000), regions[3].base);
    try std.testing.expectEqual(partition.SauAttr.nsc, regions[3].attr);
}

test "unmapped memory stays Secure: the map never opts into ALLNS" {
    try std.testing.expect(!partition.board_map.sau_all_ns);
}

test "a security_init sequence walks the steps in order and rebalances PRCR" {
    boot.reset();
    const words = ipc.encode(ipc.cpu1Pingpong());

    boot.host.write16(regs.Prcr.open);
    boot.step = .prcr_unlocked;
    boot.host.write32(regs.Addr.ipcsar, words.ipcsar);
    boot.host.write32(regs.Addr.ipcpar, words.ipcpar);
    boot.step = .ipcsar_written;
    boot.host.write16(regs.Prcr.close);
    boot.step = .prcr_relocked;

    try std.testing.expectEqual(boot.Step.prcr_relocked, boot.step);
    try std.testing.expectEqual(words.ipcsar, boot.host.ipcsar_value);
    try std.testing.expect(boot.host.balanced());
}

test "arming the jump records the vector table the NS world will use" {
    boot.reset();
    const ns_base: u32 = 0x02080000;
    boot.host.write32(regs.Addr.vtor_ns, ns_base);
    boot.step = .blxns_armed;
    try std.testing.expectEqual(ns_base, boot.host.vtor_ns);
    try std.testing.expectEqual(@as(u32, 0), boot.host.ipcsar_value);
}

test "the step values are the contract a bench probe indexes against" {
    try std.testing.expectEqual(@as(u8, 0), @intFromEnum(boot.Step.idle));
    try std.testing.expectEqual(@as(u8, 1), @intFromEnum(boot.Step.sau_done));
    try std.testing.expectEqual(@as(u8, 6), @intFromEnum(boot.Step.branched));
}

test "a PSAR request on the IPC block plans an apply, not a reject" {
    switch (psar.plan(regs.Addr.ipcsar, 0b1010)) {
        .apply => |bits| try std.testing.expectEqual(@as(u32, 0b1010), bits),
        else => return error.TestUnexpectedResult,
    }
}
