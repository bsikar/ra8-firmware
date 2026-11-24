//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Unit tests migrated from `src/internal/*.zig` for the `ra8_tz_secure_boot`
//! decision modules, capture seam, and fault-handling tables.

const std = @import("std");
const abi = @import("abi");

const boot = abi.boot;
const hal = abi.hal;
const ipc = abi.ipc;
const nsimage = abi.nsimage;
const partition = abi.partition;
const psar = abi.psar;
const regs = abi.regs;
const sau = abi.sau;

test "boot: a full gate cycle leaves the capture balanced" {
    boot.reset();
    boot.host.write16(regs.Prcr.open);
    boot.host.write32(regs.Addr.ipcsar, 0xABCD);
    boot.host.write32(regs.Addr.ipcpar, 0x1234);
    boot.host.write16(regs.Prcr.close);
    try std.testing.expectEqual(@as(u32, 0xABCD), boot.host.ipcsar_value);
    try std.testing.expectEqual(@as(u32, 0x1234), boot.host.ipcpar_value);
    try std.testing.expectEqual(@as(u8, 1), boot.host.prcr_unlock_count);
    try std.testing.expect(boot.host.balanced());
    try std.testing.expectEqual(regs.Prcr.close, boot.host.prcr_s_last);
}

test "boot: an address outside the documented set lands in the PSAR shadow" {
    boot.reset();
    boot.host.write32(0x4000_0000, 0xDEAD);
    try std.testing.expectEqual(@as(u32, 0), boot.host.ipcsar_value);
    try std.testing.expectEqual(@as(u32, 0), boot.host.vtor_ns);
    try std.testing.expectEqual(@as(u32, 0xDEAD), boot.host.read32(0x4000_0000));
}

test "boot: the PSAR shadow reads back only the address it was written at" {
    boot.reset();
    boot.host.write32(0x4000_8000, 0x30);
    try std.testing.expectEqual(@as(u32, 0x30), boot.host.read32(0x4000_8000));
    try std.testing.expectEqual(@as(u32, 0), boot.host.read32(0x4000_9000));
}

test "boot: reset clears a dirtied capture" {
    boot.reset();
    boot.host.write16(regs.Prcr.open);
    boot.host.vtor_ns = 0x2008_0000;
    boot.step = .blxns_armed;
    boot.reset();
    try std.testing.expectEqual(boot.Step.idle, boot.step);
    try std.testing.expectEqual(@as(u32, 0), boot.host.vtor_ns);
    try std.testing.expect(boot.host.balanced());
}

test "hal: the fake answers what it was told to answer" {
    hal.reset();
    hal.fake.implemented = 5;
    try std.testing.expectEqual(@as(u8, 5), hal.regionCount());
    hal.fake.configure_result = regs.Err.not_supported;
    const cfg: hal.SauCfg = .{ .regions = null, .region_count = 0, .all_ns = false };
    try std.testing.expectEqual(regs.Err.not_supported, hal.configure(&cfg));
    try std.testing.expectEqual(@as(u8, 1), hal.fake.configure_count);
}

test "hal: reset clears a dirtied fake" {
    hal.reset();
    _ = hal.setBoundary(2, 0x4000);
    try std.testing.expectEqual(@as(u8, 2), hal.fake.last_bank);
    hal.reset();
    try std.testing.expectEqual(@as(u8, 0), hal.fake.boundary_count);
    try std.testing.expectEqual(@as(u8, 8), hal.fake.implemented);
}

test "ipc: a zeroed map encodes to the cold-reset word pair" {
    const words = ipc.encode(.{});
    try std.testing.expectEqual(@as(u32, 0), words.ipcsar);
    try std.testing.expectEqual(@as(u32, 0), words.ipcpar);
}

test "ipc: each target lands on its own bit, and the two registers share the layout" {
    for (ipc.shifts, 0..) |shift, index| {
        var cfg: ipc.Attribution = .{};
        cfg.target[index].world = .non_secure;
        cfg.target[index].access = .unprivileged;
        const words = ipc.encode(cfg);
        try std.testing.expectEqual(@as(u32, 1) << shift, words.ipcsar);
        try std.testing.expectEqual(@as(u32, 1) << shift, words.ipcpar);
    }
}

test "ipc: world and access are answered independently" {
    var cfg: ipc.Attribution = .{};
    cfg.target[@backingInt(ipc.Target.sem_high)].world = .non_secure;
    const words = ipc.encode(cfg);
    try std.testing.expectEqual(@as(u32, 1) << 1, words.ipcsar);
    try std.testing.expectEqual(@as(u32, 0), words.ipcpar);
}

test "ipc: cpu1_pingpong hands out channel0 and channel2 only" {
    const words = ipc.encode(ipc.cpu1Pingpong());
    try std.testing.expectEqual((@as(u32, 1) << 16) | (@as(u32, 1) << 18), words.ipcsar);
    try std.testing.expectEqual(@as(u32, 0), words.ipcpar);
}

test "ipc: an out-of-range byte is rejected before it can be encoded" {
    var cfg: ipc.Attribution = .{};
    cfg.target[0].world = @fromBackingInt(@intCast(7));
    try std.testing.expect(!ipc.valid(cfg));
    try std.testing.expect(ipc.valid(.{}));
}

test "ipc: every shift is distinct" {
    var seen: u32 = 0;
    for (ipc.shifts) |shift| {
        const bit = @as(u32, 1) << shift;
        try std.testing.expectEqual(@as(u32, 0), seen & bit);
        seen |= bit;
    }
    _ = regs.Err.ok;
}

fn placeNsHeader(buffer: []u8, magic: u32, body_len: u32) void {
    std.mem.writeInt(u32, buffer[regs.NsRot.header_offset..][0..4], magic, .little);
    std.mem.writeInt(u32, buffer[regs.NsRot.header_offset + 4 ..][0..4], body_len, .little);
}

test "nsimage: a well-formed header reports its body length" {
    var img: [0x80]u8 = @splat(0);
    placeNsHeader(&img, regs.NsRot.magic, 0x4000);
    try std.testing.expectEqual(@as(u32, 0x4000), nsimage.signedBodyLen(&img));
}

test "nsimage: a wrong magic denies, whatever the length field says" {
    var img: [0x80]u8 = @splat(0);
    placeNsHeader(&img, 0xDEADBEEF, 0x4000);
    try std.testing.expectEqual(@as(u32, 0), nsimage.signedBodyLen(&img));
}

test "nsimage: an unwritten image denies" {
    const img: [0x80]u8 = @splat(0);
    try std.testing.expectEqual(@as(u32, 0), nsimage.signedBodyLen(&img));
}

test "nsimage: a zero body length is itself the deny sentinel" {
    var img: [0x80]u8 = @splat(0);
    placeNsHeader(&img, regs.NsRot.magic, 0);
    try std.testing.expectEqual(@as(u32, 0), nsimage.signedBodyLen(&img));
}

test "nsimage: the header sits just past the 16-slot vector table" {
    try std.testing.expectEqual(@as(usize, 16 * 4), regs.NsRot.header_offset);
    try std.testing.expectEqual(@as(usize, 8), @sizeOf(nsimage.Header));
}

test "partition: the board map validates against the silicon it is written for" {
    try std.testing.expectEqual(regs.Err.ok, partition.validate(&partition.board_map, 8));
    try std.testing.expectEqual(@as(u8, 4), partition.board_map.sau_region_count);
    try std.testing.expect(!partition.board_map.sau_all_ns);
    try std.testing.expect(partition.board_map.sram_boundary == null);
}

test "partition: a count the silicon cannot serve is not supported" {
    try std.testing.expectEqual(regs.Err.not_supported, partition.validate(&partition.board_map, 3));
    try std.testing.expectEqual(regs.Err.ok, partition.validate(&partition.board_map, 4));
}

test "partition: a non-zero count with no table is a null pointer" {
    const cfg: partition.Partition = .{
        .sau_regions = null,
        .sram_boundary = null,
        .sau_region_count = 2,
        .sau_all_ns = false,
    };
    try std.testing.expectEqual(regs.Err.null_ptr, partition.validate(&cfg, 8));
}

test "partition: an empty descriptor is legal and asks for nothing" {
    const cfg: partition.Partition = .{
        .sau_regions = null,
        .sram_boundary = null,
        .sau_region_count = 0,
        .sau_all_ns = false,
    };
    try std.testing.expectEqual(regs.Err.ok, partition.validate(&cfg, 0));
}

test "partition: base and size must both sit on the 32-byte granule" {
    try std.testing.expectEqual(regs.Err.invalid_arg, partition.validateRegion(.{
        .base = 0x02080010,
        .size = 0x80000,
        .attr = .ns,
    }));
    try std.testing.expectEqual(regs.Err.invalid_arg, partition.validateRegion(.{
        .base = 0x02080000,
        .size = 0x80010,
        .attr = .ns,
    }));
}

test "partition: a zero-size window is rejected" {
    try std.testing.expectEqual(regs.Err.invalid_arg, partition.validateRegion(.{
        .base = 0x02080000,
        .size = 0,
        .attr = .ns,
    }));
}

test "partition: a window may touch the top of the address space but not cross it" {
    try std.testing.expectEqual(regs.Err.ok, partition.validateRegion(.{
        .base = 0xFFFFF000,
        .size = 0x1000,
        .attr = .ns,
    }));
    try std.testing.expectEqual(regs.Err.invalid_arg, partition.validateRegion(.{
        .base = 0xFFFFF000,
        .size = 0x2000,
        .attr = .ns,
    }));
}

test "partition: an unnamed attribute is a caller error" {
    try std.testing.expectEqual(regs.Err.invalid_arg, partition.validateRegion(.{
        .base = 0x02080000,
        .size = 0x80000,
        .attr = @fromBackingInt(@intCast(9)),
    }));
}

test "partition: an SRAM boundary must be 4 KB aligned in every bank" {
    const good = [_]u32{ 0, 0x2000, 0x4000, 0x6000 };
    const bad = [_]u32{ 0, 0x2000, 0x2001, 0x6000 };
    var cfg = partition.board_map;
    cfg.sram_boundary = &good;
    try std.testing.expectEqual(regs.Err.ok, partition.validate(&cfg, 8));
    cfg.sram_boundary = &bad;
    try std.testing.expectEqual(regs.Err.invalid_arg, partition.validate(&cfg, 8));
}

test "psar: a null address is refused before the gate opens" {
    try std.testing.expectEqual(psar.Plan{ .reject = regs.Err.invalid_arg }, psar.plan(0, 0xF));
}

test "psar: an empty mask observes without opening the gate" {
    try std.testing.expectEqual(psar.Plan.observe, psar.plan(0x4000_0000, 0));
}

test "psar: a real request carries its bits through" {
    try std.testing.expectEqual(psar.Plan{ .apply = 0xF0 }, psar.plan(0x4000_0000, 0xF0));
}

test "psar: merging only ever adds bits" {
    try std.testing.expectEqual(@as(u32, 0b1111), psar.merged(0b1100, 0b0011));
    try std.testing.expectEqual(@as(u32, 0b1100), psar.merged(0b1100, 0b1100));
}

test "psar: a read-back settles only once every requested bit is set" {
    try std.testing.expect(psar.settled(0b1111, 0b0011));
    try std.testing.expect(!psar.settled(0b1101, 0b0011));
    try std.testing.expect(psar.settled(0, 0));
}

test "psar: the spin bound is a real bound" {
    try std.testing.expect(regs.Psar.readback_spins > 0);
}

test "regs: PRCR open carries the key and PRC4, close carries the key alone" {
    try std.testing.expectEqual(@as(u16, 0xA510), regs.Prcr.open);
    try std.testing.expectEqual(@as(u16, 0xA500), regs.Prcr.close);
    try std.testing.expectEqual(@as(u16, 0), regs.Prcr.close & regs.Prcr.prc4);
}

test "regs: NS RoT magic reads NSR1 in a byte dump" {
    var bytes: [4]u8 = undefined;
    std.mem.writeInt(u32, &bytes, regs.NsRot.magic, .little);
    try std.testing.expectEqualSlices(u8, "NSR1", &bytes);
}

test "sau: the published region order is what the table actually holds" {
    try std.testing.expectEqual(@as(usize, 5), sau.Region.count);
    try std.testing.expectEqual(@as(usize, 0x10000000), sau.window(.code_nsc).base);
    try std.testing.expectEqual(@as(usize, 0x12080000), sau.window(.ns_mram).base);
    try std.testing.expectEqual(@as(usize, 0x12000000), sau.window(.sram_nsc).base);
    try std.testing.expectEqual(@as(usize, 0x32100000), sau.window(.ns_sram).base);
    try std.testing.expectEqual(@as(usize, 0x50000000), sau.window(.ns_periph).base);
}

test "sau: only the two veneer aliases are NSC" {
    try std.testing.expectEqual(partition.SauAttr.nsc, sau.window(.code_nsc).attr);
    try std.testing.expectEqual(partition.SauAttr.nsc, sau.window(.sram_nsc).attr);
    try std.testing.expectEqual(partition.SauAttr.ns, sau.window(.ns_mram).attr);
    try std.testing.expectEqual(partition.SauAttr.ns, sau.window(.ns_sram).attr);
    try std.testing.expectEqual(partition.SauAttr.ns, sau.window(.ns_periph).attr);
}

test "sau: the reset path's own partition passes the validator that guards apply" {
    try std.testing.expectEqual(regs.Err.ok, partition.validate(&sau.descriptor, sau.Region.count));
}

test "sau: silicon short of five regions cannot hold this partition" {
    try std.testing.expect(!sau.supported(4));
    try std.testing.expect(sau.supported(5));
    try std.testing.expect(sau.supported(8));
    try std.testing.expectEqual(
        regs.Err.not_supported,
        partition.validate(&sau.descriptor, 4),
    );
}

test "sau: no two windows overlap" {
    for (sau.regions, 0..) |mine, i| {
        const my_top = mine.base + mine.size;
        for (sau.regions[i + 1 ..]) |theirs| {
            const their_top = theirs.base + theirs.size;
            try std.testing.expect(my_top <= theirs.base or their_top <= mine.base);
        }
    }
}

test "sau: unmapped memory stays Secure" {
    try std.testing.expect(!sau.all_ns);
    try std.testing.expect(!sau.descriptor.sau_all_ns);
}

test "sau: the NS image window is the upper half of MRAM, not the whole bank" {
    const ns_mram = sau.window(.ns_mram);
    try std.testing.expectEqual(@as(u32, 0x80000), ns_mram.size);
    try std.testing.expect(ns_mram.base > 0x12000000);
}

test "sau: the NS windows sit at the bit-28 Non-secure aliases" {
    const alias_bit: u32 = 1 << 28;
    try std.testing.expect(sau.window(.ns_mram).base & alias_bit != 0);
    try std.testing.expect(sau.window(.ns_sram).base & alias_bit != 0);
    try std.testing.expect(sau.window(.ns_periph).base & alias_bit != 0);
}

test "sau: the NS peripheral window covers IPC's NS alias, not its secure base" {
    const periph = sau.window(.ns_periph);
    const in_window = 0x5002_0000 >= periph.base and 0x5002_0000 < periph.base + periph.size;
    const secure_base_in_window = 0x4002_0000 >= periph.base and 0x4002_0000 < periph.base + periph.size;
    try std.testing.expect(in_window);
    try std.testing.expect(!secure_base_in_window);
}
