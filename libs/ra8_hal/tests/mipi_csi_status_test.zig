//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const st = @import("mipi_csi_status");

const Fake = struct {
    regs: [0x300 / 4]u32 = @splat(0),
    errs: u8 = 0,
    writes: u8 = 0,
    gcd_after: ?u16 = null,
    gsst_reads: u16 = 0,

    pub fn read32(f: *Fake, off: u16) u32 {
        if (off == st.off_gsst) {
            f.gsst_reads += 1;
            if (f.gcd_after) |n| {
                if (f.gsst_reads >= n) return f.regs[off / 4] | st.gsst_gcd;
            }
        }
        return f.regs[off / 4];
    }
    pub fn write32(f: *Fake, off: u16, value: u32) void {
        f.writes += 1;
        f.regs[off / 4] = value;
    }
    pub fn err(f: *Fake, _: [*:0]const u8) void {
        f.errs += 1;
    }
};

test "lane status reads the per-lane register" {
    var f = Fake{};
    f.regs[0x090 / 4] = 0xABCD;
    var out: u32 = 0;
    try std.testing.expectEqual(st.ok, st.getIndexed(&f, .lane, st.off_dlst0, 1, &out));
    try std.testing.expectEqual(@as(u32, 0xABCD), out);
}

test "lane index past dl_max is invalid and touches nothing" {
    var f = Fake{};
    var out: u32 = 7;
    try std.testing.expectEqual(st.invalid_arg, st.getIndexed(&f, .lane, st.off_dlst0, 2, &out));
    try std.testing.expectEqual(st.invalid_arg, st.writeIndexed(&f, .lane, st.off_dlsc0, 2, 1));
    try std.testing.expectEqual(@as(u32, 7), out);
    try std.testing.expectEqual(@as(u8, 0), f.writes);
}

test "null out pointer logs and returns null_ptr" {
    var f = Fake{};
    try std.testing.expectEqual(st.null_ptr, st.getIndexed(&f, .vc, st.off_vcst0, 0, null));
    try std.testing.expectEqual(st.null_ptr, st.getReg(&f, st.off_pmst, null));
    try std.testing.expectEqual(st.null_ptr, st.readShortPacket(&f, null));
    try std.testing.expectEqual(@as(u8, 3), f.errs);
}

test "vc writes land at the 0x10 stride and vc 16 is rejected" {
    var f = Fake{};
    try std.testing.expectEqual(st.ok, st.writeIndexed(&f, .vc, st.off_vcie0, 15, 0x55));
    try std.testing.expectEqual(@as(u32, 0x55), f.regs[(0x108 + 15 * 0x10) / 4]);
    try std.testing.expectEqual(st.invalid_arg, st.writeIndexed(&f, .vc, st.off_vcie0, 16, 0x55));
}

test "short packet threshold above 15 is rejected" {
    var f = Fake{};
    try std.testing.expectEqual(st.invalid_arg, st.configureShortPacket(&f, 16, true));
    try std.testing.expectEqual(@as(u8, 0), f.writes);
}

test "short packet configure packs threshold and store enable" {
    var f = Fake{};
    try std.testing.expectEqual(st.ok, st.configureShortPacket(&f, 9, true));
    try std.testing.expectEqual(@as(u32, 0x0001_0009), f.regs[st.off_gsct / 4]);
    try std.testing.expectEqual(st.ok, st.configureShortPacket(&f, 3, false));
    try std.testing.expectEqual(@as(u32, 3), f.regs[st.off_gsct / 4]);
}

test "read short packet on an empty fifo returns empty" {
    var f = Fake{};
    var pkt: st.ShortPacket = undefined;
    try std.testing.expectEqual(st.empty, st.readShortPacket(&f, &pkt));
    try std.testing.expectEqual(@as(u8, 0), f.writes);
}

test "read short packet pulses FINC and decodes the header" {
    var f = Fake{};
    f.regs[st.off_gsst / 4] = 0x0200;
    f.regs[st.off_gsht / 4] = 0x0A12_BEEF;
    var pkt: st.ShortPacket = undefined;
    try std.testing.expectEqual(st.ok, st.readShortPacket(&f, &pkt));
    try std.testing.expectEqual(st.gsiu_finc, f.regs[st.off_gsiu / 4]);
    try std.testing.expectEqual(@as(u16, 0xBEEF), pkt.payload);
    try std.testing.expectEqual(@as(u8, 0x12), pkt.data_type);
    try std.testing.expectEqual(@as(u8, 0xA), pkt.vc);
    try std.testing.expectEqual(@as(u32, 0x0A12_BEEF), pkt.raw);
}

test "clear fifo succeeds once GCD sets and releases GSIU" {
    var f = Fake{ .gcd_after = 3 };
    try std.testing.expectEqual(st.ok, st.clearFifo(&f));
    try std.testing.expectEqual(@as(u16, 3), f.gsst_reads);
    try std.testing.expectEqual(@as(u32, 0), f.regs[st.off_gsiu / 4]);
}

test "clear fifo times out after the spin budget and still releases GSIU" {
    var f = Fake{};
    try std.testing.expectEqual(st.hw_timeout, st.clearFifo(&f));
    try std.testing.expectEqual(st.gfclr_spin_max, f.gsst_reads);
    try std.testing.expectEqual(@as(u32, 0), f.regs[st.off_gsiu / 4]);
    try std.testing.expectEqual(@as(u8, 2), f.writes);
}

test "ShortPacket matches the C layout" {
    try std.testing.expectEqual(@as(usize, 8), @sizeOf(st.ShortPacket));
    try std.testing.expectEqual(@as(usize, 2), @offsetOf(st.ShortPacket, "data_type"));
    try std.testing.expectEqual(@as(usize, 4), @offsetOf(st.ShortPacket, "raw"));
}
