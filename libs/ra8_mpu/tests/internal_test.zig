//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Tests for the pure MPU validation and encoding rules. No MMIO here: every
//! case is a function of a caller-owned descriptor.

const std = @import("std");
const implementation = @import("implementation");

const Region = implementation.Region;
const Config = implementation.Config;

fn region(base: usize, size: u32, priv: u8, unpriv: u8) Region {
    return .{
        .base = base,
        .size = size,
        .priv = priv,
        .unpriv = unpriv,
        .executable = true,
        .shareable = implementation.share_non,
        .attr_idx = 0,
    };
}

test "isPow2 rejects zero" {
    try std.testing.expect(!implementation.isPow2(0));
}

test "isPow2 rejects a non-power-of-two" {
    try std.testing.expect(!implementation.isPow2(3));
    try std.testing.expect(!implementation.isPow2(0x1500));
    try std.testing.expect(!implementation.isPow2(0x000A0000));
}

test "isPow2 accepts powers of two across the range" {
    var bit: u5 = 0;
    while (bit < 31) : (bit += 1) {
        try std.testing.expect(implementation.isPow2(@as(u32, 1) << bit));
    }
    try std.testing.expect(implementation.isPow2(0x80000000));
}

test "encodeAp: priv rw, unpriv none" {
    try std.testing.expectEqual(
        implementation.ap_priv_rw_unpriv_none,
        implementation.encodeAp(implementation.perm_rw, implementation.perm_none),
    );
}

test "encodeAp: priv rw, unpriv rw" {
    try std.testing.expectEqual(
        implementation.ap_priv_rw_unpriv_rw,
        implementation.encodeAp(implementation.perm_rw, implementation.perm_rw),
    );
}

test "encodeAp: priv ro, unpriv none" {
    try std.testing.expectEqual(
        implementation.ap_priv_ro_unpriv_none,
        implementation.encodeAp(implementation.perm_ro, implementation.perm_none),
    );
}

test "encodeAp: priv ro, unpriv ro" {
    try std.testing.expectEqual(
        implementation.ap_priv_ro_unpriv_ro,
        implementation.encodeAp(implementation.perm_ro, implementation.perm_ro),
    );
}

test "encodeAp rejects priv ro with unpriv rw" {
    try std.testing.expectEqual(
        implementation.ap_invalid,
        implementation.encodeAp(implementation.perm_ro, implementation.perm_rw),
    );
}

test "encodeAp rejects priv rw with unpriv ro" {
    try std.testing.expectEqual(
        implementation.ap_invalid,
        implementation.encodeAp(implementation.perm_rw, implementation.perm_ro),
    );
}

test "encodeAp rejects a no-access privileged level" {
    try std.testing.expectEqual(
        implementation.ap_invalid,
        implementation.encodeAp(implementation.perm_none, implementation.perm_none),
    );
}

test "encodeAp rejects a permission byte outside the declared levels" {
    try std.testing.expectEqual(
        implementation.ap_invalid,
        implementation.encodeAp(0xAA, implementation.perm_rw),
    );
}

test "encodeAp sweeps every byte pair and only the four encodable pairs pass" {
    var priv: u16 = 0;
    var encodable: usize = 0;
    while (priv <= 0xFF) : (priv += 1) {
        var unpriv: u16 = 0;
        while (unpriv <= 0xFF) : (unpriv += 1) {
            const code = implementation.encodeAp(@truncate(priv), @truncate(unpriv));
            if (code != implementation.ap_invalid) encodable += 1;
        }
    }
    try std.testing.expectEqual(@as(usize, 4), encodable);
}

test "checkRegion accepts the minimum legal region" {
    const r = region(0x20000000, 32, implementation.perm_rw, implementation.perm_rw);
    try std.testing.expectEqual(implementation.RegionFault.ok, implementation.checkRegion(&r));
}

test "checkRegion rejects size zero" {
    const r = region(0x20000000, 0, implementation.perm_rw, implementation.perm_rw);
    try std.testing.expectEqual(implementation.RegionFault.bad_size, implementation.checkRegion(&r));
}

test "checkRegion rejects a non-power-of-two size" {
    const r = region(0x20000000, 3, implementation.perm_rw, implementation.perm_rw);
    try std.testing.expectEqual(implementation.RegionFault.bad_size, implementation.checkRegion(&r));
}

test "checkRegion rejects a power of two below the architectural minimum" {
    const r = region(0x20000000, 16, implementation.perm_rw, implementation.perm_rw);
    try std.testing.expectEqual(implementation.RegionFault.bad_size, implementation.checkRegion(&r));
}

test "checkRegion rejects a base misaligned to the region size" {
    const r = region(0x20000010, 0x1000, implementation.perm_rw, implementation.perm_rw);
    try std.testing.expectEqual(implementation.RegionFault.misaligned, implementation.checkRegion(&r));
}

test "checkRegion rejects an unencodable permission pair" {
    const r = region(0x20000000, 0x1000, implementation.perm_ro, implementation.perm_rw);
    try std.testing.expectEqual(
        implementation.RegionFault.unencodable_perms,
        implementation.checkRegion(&r),
    );
}

test "checkRegion reports the size rule before the alignment rule" {
    // Misaligned AND not a power of two: the C checked size first.
    const r = region(0x20000010, 3, implementation.perm_rw, implementation.perm_rw);
    try std.testing.expectEqual(implementation.RegionFault.bad_size, implementation.checkRegion(&r));
}

test "checkRegion reports the alignment rule before the permission rule" {
    const r = region(0x20000010, 0x1000, implementation.perm_ro, implementation.perm_rw);
    try std.testing.expectEqual(implementation.RegionFault.misaligned, implementation.checkRegion(&r));
}

test "buildRbar packs base, shareability, AP and XN" {
    var r = region(0x20000000, 0x1000, implementation.perm_rw, implementation.perm_none);
    r.executable = false;
    r.shareable = implementation.share_inner;
    const rbar = implementation.buildRbar(&r);
    try std.testing.expectEqual(@as(u32, 0x20000000), rbar & implementation.rbar_base_mask);
    try std.testing.expectEqual(
        @as(u32, implementation.ap_priv_rw_unpriv_none),
        (rbar & implementation.rbar_ap_mask) >> implementation.rbar_ap_shift,
    );
    try std.testing.expectEqual(
        @as(u32, implementation.share_inner),
        (rbar & implementation.rbar_sh_mask) >> implementation.rbar_sh_shift,
    );
    try std.testing.expectEqual(implementation.rbar_xn_mask, rbar & implementation.rbar_xn_mask);
}

test "buildRbar clears XN for an executable region" {
    const r = region(0x20000000, 0x1000, implementation.perm_ro, implementation.perm_ro);
    const rbar = implementation.buildRbar(&r);
    try std.testing.expectEqual(@as(u32, 0), rbar & implementation.rbar_xn_mask);
    try std.testing.expectEqual(
        @as(u32, 3),
        (rbar & implementation.rbar_ap_mask) >> implementation.rbar_ap_shift,
    );
}

test "buildRbar masks the low base bits the architecture reserves" {
    var r = region(0x20000000, 0x1000, implementation.perm_rw, implementation.perm_rw);
    r.base = 0x2000001F;
    try std.testing.expectEqual(
        @as(u32, 0x20000000),
        implementation.buildRbar(&r) & implementation.rbar_base_mask,
    );
}

test "buildRlar packs the inclusive limit, AttrIdx and EN" {
    var r = region(0x20000000, 0x1000, implementation.perm_rw, implementation.perm_rw);
    r.attr_idx = 3;
    const rlar = implementation.buildRlar(&r);
    try std.testing.expectEqual(@as(u32, 0x20000FE0), rlar & implementation.rlar_limit_mask);
    try std.testing.expectEqual(
        @as(u32, 3),
        (rlar & implementation.rlar_attridx_mask) >> implementation.rlar_attridx_shift,
    );
    try std.testing.expectEqual(implementation.rlar_en_mask, rlar & implementation.rlar_en_mask);
}

test "buildRlar encodes the 640 KiB shared bank the size check would reject" {
    const shram = implementation.boot_regions[4];
    try std.testing.expectEqual(@as(u32, 0x22100003), implementation.buildRbar(&shram));
    try std.testing.expectEqual(@as(u32, 0x2219FFE3), implementation.buildRlar(&shram));
    try std.testing.expectEqual(
        @as(u32, 0x2219FFE0),
        implementation.buildRlar(&shram) & implementation.rlar_limit_mask,
    );
}

test "buildCtrl always sets ENABLE" {
    const cfg = Config{
        .regions = null,
        .region_count = 0,
        .mair0 = 0,
        .mair1 = 0,
        .privdefena = false,
        .hfnmiena = false,
    };
    try std.testing.expectEqual(implementation.ctrl_enable, implementation.buildCtrl(&cfg));
}

test "buildCtrl folds in PRIVDEFENA and HFNMIENA" {
    var cfg = Config{
        .regions = null,
        .region_count = 0,
        .mair0 = 0,
        .mair1 = 0,
        .privdefena = true,
        .hfnmiena = false,
    };
    try std.testing.expectEqual(
        implementation.ctrl_enable | implementation.ctrl_privdefena,
        implementation.buildCtrl(&cfg),
    );
    cfg.hfnmiena = true;
    try std.testing.expectEqual(
        implementation.ctrl_enable | implementation.ctrl_privdefena | implementation.ctrl_hfnmiena,
        implementation.buildCtrl(&cfg),
    );
}

test "dregionOf decodes MPU_TYPE bits 15:8" {
    try std.testing.expectEqual(@as(u8, 16), implementation.dregionOf(16 << 8));
    try std.testing.expectEqual(@as(u8, 8), implementation.dregionOf(8 << 8));
    try std.testing.expectEqual(@as(u8, 0), implementation.dregionOf(0));
    try std.testing.expectEqual(@as(u8, 0xFF), implementation.dregionOf(0xFFFFFFFF));
}

test "boot map holds exactly five regions" {
    try std.testing.expectEqual(
        @as(usize, implementation.boot_region_count),
        implementation.boot_regions.len,
    );
}

test "boot map region 0 is executable read-only MRAM code" {
    const r = implementation.boot_regions[0];
    try std.testing.expectEqual(@as(usize, 0x02000000), r.base);
    try std.testing.expectEqual(@as(u32, 0x00100000), r.size);
    try std.testing.expectEqual(implementation.perm_ro, r.priv);
    try std.testing.expectEqual(implementation.perm_ro, r.unpriv);
    try std.testing.expect(r.executable);
    try std.testing.expectEqual(@as(u8, 0), r.attr_idx);
}

test "boot map region 3 is the Device-nGnRE peripheral window" {
    const r = implementation.boot_regions[3];
    try std.testing.expectEqual(@as(usize, 0x40000000), r.base);
    try std.testing.expectEqual(implementation.perm_rw, r.priv);
    try std.testing.expect(!r.executable);
    try std.testing.expectEqual(@as(u8, 2), r.attr_idx);
}

test "boot map region 4 is the non-power-of-two shared bank" {
    const r = implementation.boot_regions[4];
    try std.testing.expectEqual(@as(usize, 0x22100000), r.base);
    try std.testing.expectEqual(@as(u32, 0x000A0000), r.size);
    try std.testing.expectEqual(@as(u8, 1), r.attr_idx);
    try std.testing.expect(!r.executable);
    try std.testing.expect(!implementation.isPow2(r.size));
    try std.testing.expectEqual(implementation.RegionFault.bad_size, implementation.checkRegion(&r));
}

test "every other boot region passes the descriptor rules" {
    for (implementation.boot_regions[0..4]) |r| {
        try std.testing.expectEqual(implementation.RegionFault.ok, implementation.checkRegion(&r));
    }
}

test "every boot region base and size sits on the 32-byte quantum" {
    for (implementation.boot_regions) |r| {
        try std.testing.expectEqual(@as(usize, 0), r.base & (implementation.min_region_size - 1));
        try std.testing.expectEqual(@as(u32, 0), r.size & (implementation.min_region_size - 1));
        try std.testing.expect(r.size >= implementation.min_region_size);
    }
}

test "boot MAIR words carry the three attribute sets the map uses" {
    try std.testing.expectEqual(@as(u32, 0x000444FF), implementation.boot_mair0);
    try std.testing.expectEqual(@as(u32, 0), implementation.boot_mair1);
    for (implementation.boot_regions) |r| {
        try std.testing.expect(r.attr_idx < 4);
    }
}

test "region layout matches the C ABI in pointer-width multiples" {
    const ptr = @sizeOf(usize);
    try std.testing.expectEqual(@as(usize, 0), @offsetOf(Region, "base"));
    try std.testing.expectEqual(ptr, @offsetOf(Region, "size"));
    try std.testing.expectEqual(ptr + 4, @offsetOf(Region, "priv"));
    try std.testing.expectEqual(ptr + 8, @offsetOf(Region, "attr_idx"));
}

test "config layout matches the C ABI in pointer-width multiples" {
    const ptr = @sizeOf(usize);
    try std.testing.expectEqual(@as(usize, 0), @offsetOf(Config, "regions"));
    try std.testing.expectEqual(ptr, @offsetOf(Config, "region_count"));
    try std.testing.expectEqual(ptr + 4, @offsetOf(Config, "mair0"));
    try std.testing.expectEqual(ptr + 8, @offsetOf(Config, "mair1"));
    try std.testing.expectEqual(ptr + 12, @offsetOf(Config, "privdefena"));
    try std.testing.expectEqual(ptr + 13, @offsetOf(Config, "hfnmiena"));
}
