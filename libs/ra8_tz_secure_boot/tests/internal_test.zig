//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Tests for the pure core: the partition invariants the C host suite asserts
//! by hand, the field encodings, the guard predicates and the host capture
//! model.

const std = @import("std");
const implementation = @import("implementation");

test "partition programmes exactly five regions in index order" {
    try std.testing.expectEqual(@as(usize, 5), implementation.partition.len);
    for (implementation.partition, 0..) |spec, i| {
        try std.testing.expectEqual(@as(u8, @intCast(i)), spec.index);
    }
}

test "partition matches the canonical RA8D2 layout" {
    try std.testing.expectEqual(@as(u32, 0x10000000), implementation.partition[0].base);
    try std.testing.expectEqual(@as(u32, 0x100FFFE0), implementation.partition[0].limit);
    try std.testing.expectEqual(@as(u32, 0x02080000), implementation.partition[1].base);
    try std.testing.expectEqual(@as(u32, 0x020FFFE0), implementation.partition[1].limit);
    try std.testing.expectEqual(@as(u32, 0x12000000), implementation.partition[2].base);
    try std.testing.expectEqual(@as(u32, 0x1200FFE0), implementation.partition[2].limit);
    try std.testing.expectEqual(@as(u32, 0x22100000), implementation.partition[3].base);
    try std.testing.expectEqual(@as(u32, 0x221FFFE0), implementation.partition[3].limit);
    try std.testing.expectEqual(@as(u32, 0x50000000), implementation.partition[4].base);
    try std.testing.expectEqual(@as(u32, 0x5FFFFFE0), implementation.partition[4].limit);
}

test "only the two alias regions are Non-Secure Callable" {
    try std.testing.expect(implementation.partition[0].is_nsc);
    try std.testing.expect(!implementation.partition[1].is_nsc);
    try std.testing.expect(implementation.partition[2].is_nsc);
    try std.testing.expect(!implementation.partition[3].is_nsc);
    try std.testing.expect(!implementation.partition[4].is_nsc);
}

test "no two programmed regions overlap" {
    try std.testing.expect(implementation.partitionDisjoint());
}

test "regions are ordered so each limit precedes the next base" {
    try std.testing.expect(implementation.partition[1].limit < implementation.partition[2].base);
    try std.testing.expect(implementation.partition[2].limit < implementation.partition[3].base);
    try std.testing.expect(implementation.partition[3].limit < implementation.partition[4].base);
}

test "every base and limit sits on the 32-byte SAU quantum" {
    try std.testing.expect(implementation.partitionQuantumAligned());
}

test "NSC aliases stay outside the Secure lower MRAM block" {
    try std.testing.expect(implementation.nscAliasesOutsideSecureMram());
}

test "NS peripheral region covers the whole 0x5 alias window" {
    const region = implementation.partition[4];
    try std.testing.expectEqual(@as(u32, 0x50000000), region.base);
    try std.testing.expect(region.limit > 0x5FFF0000);
}

test "rlarFor sets ENABLE on a plain Non-Secure region" {
    try std.testing.expectEqual(@as(u32, 0x020FFFE1), implementation.rlarFor(0x020FFFE0, false));
}

test "rlarFor sets ENABLE and NSC on a callable region" {
    try std.testing.expectEqual(@as(u32, 0x100FFFE3), implementation.rlarFor(0x100FFFE0, true));
}

test "rlarFor leaves the limit bits untouched" {
    const limit: u32 = 0x22100000;
    const rlar = implementation.rlarFor(limit, false);
    try std.testing.expectEqual(limit, rlar & ~@as(u32, 0x1F));
}

test "sauRegionsSufficient accepts the Cortex-M85's eight regions" {
    try std.testing.expect(implementation.sauRegionsSufficient(8));
}

test "sauRegionsSufficient accepts exactly five" {
    try std.testing.expect(implementation.sauRegionsSufficient(5));
}

test "sauRegionsSufficient rejects four" {
    try std.testing.expect(!implementation.sauRegionsSufficient(4));
}

test "sauRegionsSufficient rejects zero" {
    try std.testing.expect(!implementation.sauRegionsSufficient(0));
}

test "sauRegionsSufficient reads only the SREGION byte" {
    // High bits are other TYPE fields; a chip reporting 3 regions stays
    // unsupported no matter what sits above the low byte.
    try std.testing.expect(!implementation.sauRegionsSufficient(0xFFFF_FF03));
    try std.testing.expect(implementation.sauRegionsSufficient(0xFFFF_FF08));
}

test "sauRegionsSufficient boundary sweep over the SREGION byte" {
    var value: u32 = 0;
    while (value < 256) : (value += 1) {
        const expected = value >= implementation.region_count;
        try std.testing.expectEqual(expected, implementation.sauRegionsSufficient(value));
    }
}

test "prcrOpensGate MC/DC vector 1: PRC4 set is an unlock" {
    try std.testing.expect(implementation.prcrOpensGate(implementation.prcr_s_open));
}

test "prcrOpensGate MC/DC vector 2: PRC4 clear is a relock" {
    try std.testing.expect(!implementation.prcrOpensGate(implementation.prcr_s_close));
}

test "PRCR_S values carry the 0xA5 write key" {
    try std.testing.expectEqual(implementation.prcr_s_key, implementation.prcr_s_open & 0xFF00);
    try std.testing.expectEqual(implementation.prcr_s_key, implementation.prcr_s_close & 0xFF00);
}

test "PRCR_S open differs from close only in PRC4" {
    const diff = implementation.prcr_s_open ^ implementation.prcr_s_close;
    try std.testing.expectEqual(implementation.prcr_s_prc4_open, diff);
}

test "resetEntryBogus MC/DC vector 1: a real vector is accepted" {
    try std.testing.expect(!implementation.resetEntryBogus(0x02080101));
}

test "resetEntryBogus MC/DC vector 2: all-zero is refused" {
    try std.testing.expect(implementation.resetEntryBogus(0));
}

test "resetEntryBogus MC/DC vector 3: erased MRAM is refused" {
    try std.testing.expect(implementation.resetEntryBogus(0xFFFFFFFF));
}

test "resetEntryBogus accepts the neighbours of both sentinels" {
    try std.testing.expect(!implementation.resetEntryBogus(1));
    try std.testing.expect(!implementation.resetEntryBogus(0xFFFFFFFE));
}

test "pointerAligned4 accepts word-aligned addresses" {
    try std.testing.expect(implementation.pointerAligned4(0x22180000));
    try std.testing.expect(implementation.pointerAligned4(0));
}

test "pointerAligned4 refuses every non-word offset" {
    try std.testing.expect(!implementation.pointerAligned4(1));
    try std.testing.expect(!implementation.pointerAligned4(2));
    try std.testing.expect(!implementation.pointerAligned4(3));
    try std.testing.expect(implementation.pointerAligned4(4));
}

test "nsEntryFromResetVector strips the Thumb bit" {
    try std.testing.expectEqual(@as(u32, 0x02080100), implementation.nsEntryFromResetVector(0x02080101));
}

test "nsEntryFromResetVector leaves an already-even entry alone" {
    try std.testing.expectEqual(@as(u32, 0x02080100), implementation.nsEntryFromResetVector(0x02080100));
}

test "headerMagicOk accepts the ASCII NSR1 marker" {
    try std.testing.expect(implementation.headerMagicOk(0x3152534E));
    try std.testing.expectEqual(@as(u32, 0x3152534E), implementation.ns_rot_header_magic);
}

test "headerMagicOk refuses an inverted marker" {
    try std.testing.expect(!implementation.headerMagicOk(implementation.ns_rot_header_magic ^ 0xFFFFFFFF));
    try std.testing.expect(!implementation.headerMagicOk(0));
}

test "NS RoT header sits past the sixteen-slot NS vector table" {
    try std.testing.expectEqual(@as(usize, 0x40), implementation.ns_rot_header_offset);
    try std.testing.expectEqual(@as(usize, 8), @sizeOf(implementation.NsRotHeader));
}

test "register addresses match the documented map" {
    try std.testing.expectEqual(@as(usize, 0xE000EDD0), implementation.Addr.sau_ctrl);
    try std.testing.expectEqual(@as(usize, 0xE000EDD4), implementation.Addr.sau_type);
    try std.testing.expectEqual(@as(usize, 0xE000EDD8), implementation.Addr.sau_rnr);
    try std.testing.expectEqual(@as(usize, 0xE000EDDC), implementation.Addr.sau_rbar);
    try std.testing.expectEqual(@as(usize, 0xE000EDE0), implementation.Addr.sau_rlar);
    try std.testing.expectEqual(@as(usize, 0xE002ED08), implementation.Addr.scb_vtor_ns);
    try std.testing.expectEqual(@as(usize, 0x40008610), implementation.Addr.ipcsar);
    try std.testing.expectEqual(@as(usize, 0x40008614), implementation.Addr.ipcpar);
    try std.testing.expectEqual(@as(usize, 0x4001E3FA), implementation.Addr.prcr_s);
}

test "SAU_CTRL enable does not raise ALLNS" {
    try std.testing.expectEqual(@as(u32, 0), implementation.sau_ctrl_enable & implementation.sau_ctrl_allns);
}

test "step values follow the documented boot order" {
    try std.testing.expectEqual(@as(u8, 0), @intFromEnum(implementation.Step.idle));
    try std.testing.expectEqual(@as(u8, 1), @intFromEnum(implementation.Step.sau_done));
    try std.testing.expectEqual(@as(u8, 2), @intFromEnum(implementation.Step.prcr_unlocked));
    try std.testing.expectEqual(@as(u8, 3), @intFromEnum(implementation.Step.ipcsar_written));
    try std.testing.expectEqual(@as(u8, 4), @intFromEnum(implementation.Step.prcr_relocked));
    try std.testing.expectEqual(@as(u8, 5), @intFromEnum(implementation.Step.blxns_armed));
    try std.testing.expectEqual(@as(u8, 6), @intFromEnum(implementation.Step.branched));
}

test "host state starts cleared" {
    const state = implementation.HostState{};
    try std.testing.expectEqual(@as(u32, 0), state.sau_ctrl);
    try std.testing.expectEqual(@as(u32, 0), state.ipcsar_value);
    try std.testing.expectEqual(@as(u8, 0), state.prcr_unlock_count);
}

test "host write32 routes each address to its own capture" {
    var state = implementation.HostState{};
    state.write32(implementation.Addr.ipcsar, 0x00050000);
    state.write32(implementation.Addr.ipcpar, 0x11);
    state.write32(implementation.Addr.scb_vtor_ns, 0x22180000);
    state.write32(implementation.Addr.sau_ctrl, implementation.sau_ctrl_enable);
    try std.testing.expectEqual(@as(u32, 0x00050000), state.ipcsar_value);
    try std.testing.expectEqual(@as(u32, 0x11), state.ipcpar_value);
    try std.testing.expectEqual(@as(u32, 0x22180000), state.vtor_ns);
    try std.testing.expectEqual(@as(u32, 1), state.sau_ctrl);
}

test "host write32 ignores the RBAR and RLAR addresses" {
    var state = implementation.HostState{};
    state.write32(implementation.Addr.sau_rbar, 0xDEADBEEF);
    state.write32(implementation.Addr.sau_rlar, 0xDEADBEEF);
    state.write32(implementation.Addr.sau_rnr, 3);
    try std.testing.expectEqual(@as(u32, 0), state.sau_ctrl);
    try std.testing.expectEqual(@as(u32, 0), state.vtor_ns);
}

test "host read32 reports eight SAU regions and reads IPCSAR back" {
    var state = implementation.HostState{};
    try std.testing.expectEqual(@as(u32, 8), state.read32(implementation.Addr.sau_type));
    state.write32(implementation.Addr.ipcsar, 0x00050000);
    try std.testing.expectEqual(@as(u32, 0x00050000), state.read32(implementation.Addr.ipcsar));
    try std.testing.expectEqual(@as(u32, 0), state.read32(implementation.Addr.sau_ctrl));
}

test "host write16 counts unlocks and relocks apart" {
    var state = implementation.HostState{};
    state.write16(implementation.Addr.prcr_s, implementation.prcr_s_open);
    try std.testing.expectEqual(@as(u8, 1), state.prcr_unlock_count);
    try std.testing.expectEqual(@as(u8, 0), state.prcr_relock_count);
    state.write16(implementation.Addr.prcr_s, implementation.prcr_s_close);
    try std.testing.expectEqual(@as(u8, 1), state.prcr_unlock_count);
    try std.testing.expectEqual(@as(u8, 1), state.prcr_relock_count);
    try std.testing.expectEqual(implementation.prcr_s_close, state.prcr_s_last);
}

test "host unlock counter wraps in a byte as the C's cast did" {
    var state = implementation.HostState{};
    var i: usize = 0;
    while (i < 256) : (i += 1) state.write16(implementation.Addr.prcr_s, implementation.prcr_s_open);
    try std.testing.expectEqual(@as(u8, 0), state.prcr_unlock_count);
}

test "host noteRegion records base, limit and the NSC flag" {
    var state = implementation.HostState{};
    state.noteRegion(2, 0x12000000, 0x1200FFE0, true);
    try std.testing.expectEqual(@as(u32, 0x12000000), state.sau_region_base[2]);
    try std.testing.expectEqual(@as(u32, 0x1200FFE0), state.sau_region_limit[2]);
    try std.testing.expectEqual(@as(u8, 1), state.sau_region_nsc[2]);
}

test "host noteRegion drops an out-of-range index" {
    var state = implementation.HostState{};
    state.noteRegion(implementation.region_count, 0x1, 0x2, true);
    state.noteRegion(255, 0x1, 0x2, true);
    for (state.sau_region_base) |base| try std.testing.expectEqual(@as(u32, 0), base);
}

test "host reset clears every capture" {
    var state = implementation.HostState{};
    state.write32(implementation.Addr.ipcsar, 0x00050000);
    state.write16(implementation.Addr.prcr_s, implementation.prcr_s_open);
    state.noteRegion(0, 0x10000000, 0x100FFFE0, true);
    state.blxns_target = 0x02080101;
    state.reset();
    try std.testing.expectEqual(@as(u32, 0), state.ipcsar_value);
    try std.testing.expectEqual(@as(u8, 0), state.prcr_unlock_count);
    try std.testing.expectEqual(@as(u32, 0), state.sau_region_base[0]);
    try std.testing.expectEqual(@as(u32, 0), state.blxns_target);
}
