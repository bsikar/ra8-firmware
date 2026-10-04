//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const sau = @import("sau");

/// Host RAM standing in for the SAU register block.
const Fake = struct {
    regs: sau.Regs = .{ .ctrl = 0, .type = 0, .rnr = 0, .rbar = 0, .rlar = 0 },

    fn block(f: *Fake) sau.Block {
        return .{ .base = @intFromPtr(&f.regs) };
    }
};

fn region(base: usize, size: u32, attr: u8) sau.Region {
    return .{ .base = base, .size = size, .attr = attr };
}

test "region validation follows the granule, the attribute and the 32-bit space" {
    try std.testing.expect(sau.regionValid(&region(0x22100000, 0x100000, sau.attr_ns)));
    try std.testing.expect(sau.regionValid(&region(0x10000000, 32, sau.attr_nsc)));
    try std.testing.expect(!sau.regionValid(&region(0x22100000, 0x100000, 2)));
    try std.testing.expect(!sau.regionValid(&region(0x22100000, 16, sau.attr_ns)));
    try std.testing.expect(!sau.regionValid(&region(0x22100000, 48, sau.attr_ns)));
    try std.testing.expect(!sau.regionValid(&region(0x22100010, 64, sau.attr_ns)));
    try std.testing.expect(!sau.regionValid(&region(0xFFFFFFE0, 64, sau.attr_ns)));
    try std.testing.expect(sau.regionValid(&region(0xFFFFFFE0, 32, sau.attr_ns)));
    // The C's headroom wraps to 0 at base 0, so base 0 is always refused.
    try std.testing.expect(!sau.regionValid(&region(0, 32, sau.attr_ns)));
}

test "descriptor validation reports null and out-of-range the way the C did" {
    try std.testing.expectError(error.NullPtr, sau.cfgValid(null, 8));
    const no_regions = sau.Cfg{ .regions = null, .region_count = 1, .all_ns = false };
    try std.testing.expectError(error.NullPtr, sau.cfgValid(&no_regions, 8));
    try std.testing.expectError(error.InvalidArg, sau.cfgValid(&sau.boot_cfg, 3));
    const bad = [_]sau.Region{region(0x22100000, 16, sau.attr_ns)};
    const bad_cfg = sau.Cfg{ .regions = &bad, .region_count = 1, .all_ns = false };
    try std.testing.expectError(error.InvalidArg, sau.cfgValid(&bad_cfg, 8));
    const empty = sau.Cfg{ .regions = null, .region_count = 0, .all_ns = true };
    _ = try sau.cfgValid(&empty, 0);
    _ = try sau.cfgValid(&sau.boot_cfg, 8);
}

test "a region programs RBAR and an inclusive RLAR with the NSC bit" {
    var f = Fake{};
    const nsc = region(0x10000000, 0x100000, sau.attr_nsc);
    f.block().writeRegion(3, &nsc);
    try std.testing.expectEqual(@as(u32, 3), f.regs.rnr);
    try std.testing.expectEqual(@as(u32, 0x10000000), f.regs.rbar);
    try std.testing.expectEqual(@as(u32, 0x100FFFE0 | sau.rlar_enable | sau.rlar_nsc), f.regs.rlar);
    const ns = region(0x6A000000, 0x02000000, sau.attr_ns);
    try std.testing.expectEqual(@as(u32, 0x6BFFFFE0 | sau.rlar_enable), sau.rlarFor(&ns));
}

test "install clears unused regions and enables with ALLNS on request" {
    var f = Fake{};
    f.regs.type = 2;
    f.regs.rlar = 0xDEAD;
    const one = [_]sau.Region{region(0x22100000, 0x100000, sau.attr_ns)};
    const cfg = sau.Cfg{ .regions = &one, .region_count = 1, .all_ns = true };
    f.block().install(&cfg);
    try std.testing.expectEqual(@as(u32, 1), f.regs.rnr);
    try std.testing.expectEqual(@as(u32, 0), f.regs.rlar);
    try std.testing.expectEqual(@as(u32, 0), f.regs.rbar);
    try std.testing.expectEqual(sau.ctrl_enable | sau.ctrl_allns, f.regs.ctrl);
    try std.testing.expectEqual(@as(u8, 2), f.block().sregionCount());
}

test "enable and disable touch only CTRL.ENABLE; the boot map is the documented four" {
    var f = Fake{};
    f.regs.ctrl = sau.ctrl_allns;
    f.block().setEnabled(true);
    try std.testing.expect(f.block().isEnabled());
    try std.testing.expectEqual(sau.ctrl_allns | sau.ctrl_enable, f.regs.ctrl);
    f.block().setEnabled(false);
    try std.testing.expect(!f.block().isEnabled());
    try std.testing.expectEqual(sau.ctrl_allns, f.regs.ctrl);
    try std.testing.expectEqual(@as(u8, 4), sau.boot_cfg.region_count);
    try std.testing.expectEqual(@as(usize, 0x10000000), sau.boot_regions[3].base);
    try std.testing.expectEqual(sau.attr_nsc, sau.boot_regions[3].attr);
    try std.testing.expectEqual(@as(u32, 0x00080000), sau.boot_regions[0].size);
}
