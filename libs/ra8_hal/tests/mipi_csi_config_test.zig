//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const cfg = @import("mipi_csi_config");

const Fake = struct {
    regs: [0x80 / 4]u32 = @splat(0),
    writes: u8 = 0,
    errs: u8 = 0,
    last_code: u16 = 0,

    pub fn read32(f: *Fake, off: u16) u32 {
        return f.regs[off / 4];
    }
    pub fn write32(f: *Fake, off: u16, value: u32) void {
        f.writes += 1;
        f.regs[off / 4] = value;
    }
    pub fn errVal(f: *Fake, _: [*:0]const u8, code: u16) void {
        f.errs += 1;
        f.last_code = code;
    }
    fn running() Fake {
        var f = Fake{};
        f.regs[cfg.off_mct3 / 4] = cfg.mct3_rxen;
        return f;
    }
};

test "data type filter writes DTEL and DTEH even while running" {
    var f = Fake.running();
    try std.testing.expectEqual(cfg.ok, cfg.setDataTypeFilter(&f, 0x1234, 0xABCD));
    try std.testing.expectEqual(@as(u32, 0x1234), f.regs[cfg.off_dtel / 4]);
    try std.testing.expectEqual(@as(u32, 0xABCD), f.regs[cfg.off_dteh / 4]);
}

test "ecc mode sets its MCT0 bits and keeps the rest" {
    var f = Fake{};
    f.regs[cfg.off_mct0 / 4] = 0x0000_0003 | cfg.mct0_lfsren;
    try std.testing.expectEqual(cfg.ok, cfg.setEccMode(&f, true, false));
    try std.testing.expectEqual(@as(u32, 0x0000_0003) | cfg.mct0_eccv13, f.regs[cfg.off_mct0 / 4]);
}

test "every setter refuses with invalid_state while RXEN is set" {
    var f = Fake.running();
    try std.testing.expectEqual(cfg.invalid_state, cfg.setEccMode(&f, true, true));
    try std.testing.expectEqual(cfg.invalid_state, cfg.setFrameErrorMode(&f, true, true, true));
    try std.testing.expectEqual(cfg.invalid_state, cfg.setEpd(&f, true, true, 1, 1));
    try std.testing.expectEqual(cfg.invalid_state, cfg.setLrte(&f, 1, true));
    try std.testing.expectEqual(@as(u8, 0), f.writes);
    try std.testing.expectEqual(@as(u8, 4), f.errs);
    try std.testing.expectEqual(cfg.invalid_state, f.last_code);
}

test "frame error mode clears and sets zlmd edmd rvmd" {
    var f = Fake{};
    f.regs[cfg.off_mct0 / 4] = cfg.mct0_zlmd | cfg.mct0_rvmd | 0x10;
    try std.testing.expectEqual(cfg.ok, cfg.setFrameErrorMode(&f, false, true, false));
    try std.testing.expectEqual(cfg.mct0_edmd | 0x10, f.regs[cfg.off_mct0 / 4]);
}

test "epd rejects oversized spacers before reading RXEN" {
    var f = Fake.running();
    try std.testing.expectEqual(cfg.invalid_arg, cfg.setEpd(&f, true, false, 0x8000, 0));
    try std.testing.expectEqual(cfg.invalid_arg, cfg.setEpd(&f, true, false, 0, 0x8000));
    try std.testing.expectEqual(@as(u8, 0), f.errs);
}

test "epd packs spacers and flags into EPCT" {
    var f = Fake{};
    f.regs[cfg.off_epct / 4] = 0xFFFF_FFFF;
    try std.testing.expectEqual(cfg.ok, cfg.setEpd(&f, true, true, 0x7FFF, 0x0123));
    try std.testing.expectEqual(@as(u32, 0x8123_FFFF), f.regs[cfg.off_epct / 4]);
    try std.testing.expectEqual(cfg.ok, cfg.setEpd(&f, false, false, 5, 6));
    try std.testing.expectEqual(@as(u32, 0x0006_0005), f.regs[cfg.off_epct / 4]);
}

test "lrte rejects vlsien above x4" {
    var f = Fake{};
    try std.testing.expectEqual(cfg.invalid_arg, cfg.setLrte(&f, 4, false));
    try std.testing.expectEqual(@as(u8, 0), f.writes);
}

test "lrte updates vlsien and eotpen and keeps other EMCT bits" {
    var f = Fake{};
    f.regs[cfg.off_emct / 4] = 0x0000_0071;
    try std.testing.expectEqual(cfg.ok, cfg.setLrte(&f, 2, false));
    try std.testing.expectEqual(@as(u32, 0x0000_0021), f.regs[cfg.off_emct / 4]);
    try std.testing.expectEqual(cfg.ok, cfg.setLrte(&f, 3, true));
    try std.testing.expectEqual(@as(u32, 0x0000_0071), f.regs[cfg.off_emct / 4]);
}
