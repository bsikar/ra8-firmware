//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/spi_b_target.zig.

const std = @import("std");
const tgt = @import("spi_b_target");

/// Two 256-byte channel banks; SPSR reads report `ready` after `busy` polls.
const Regs = struct {
    mem: [2][64]u32 = @splat(@as([64]u32, @splat(0))),
    busy: u32 = 0,
    ready: u32 = 0xA000_0000,
    rx_byte: u32 = 0x1A5,
    writes: u32 = 0,

    pub fn read32(self: *Regs, ch: u8, off: usize) u32 {
        if (off == tgt.off_spsr) {
            if (self.busy > 0) {
                self.busy -= 1;
                return 0;
            }
            return self.ready;
        }
        if (off == tgt.off_spdr) return self.rx_byte;
        return self.mem[ch][off / 4];
    }
    pub fn write32(self: *Regs, ch: u8, off: usize, v: u32) void {
        self.mem[ch][off / 4] = v;
        self.writes += 1;
    }
    fn at(self: *const Regs, ch: u8, off: usize) u32 {
        return self.mem[ch][off / 4];
    }
};

const Svc = struct {
    mstp_err: u16 = 0,
    mstp_id: u16 = 0,
    last: [*:0]const u8 = "",
    info_val: u32 = 0xFF,

    pub fn mstpEnable(self: *Svc, id: u16) u16 {
        self.mstp_id = id;
        return self.mstp_err;
    }
    pub fn infoVal(self: *Svc, _: [*:0]const u8, v: u32) void {
        self.info_val = v;
    }
    pub fn err(self: *Svc, msg: [*:0]const u8) void {
        self.last = msg;
    }
    pub fn fail(self: *Svc, msg: [*:0]const u8, _: u16) void {
        self.last = msg;
    }
};

fn cfg(mode: u8, lsb: bool) tgt.Cfg {
    return .{ .baud_hz = 1_000_000, .pclka_hz = 100_000_000, .mode = mode, .lsb_first = lsb, .loopback = false };
}

test "Cfg matches ra8_spi_cfg_t" {
    try std.testing.expectEqual(@as(usize, 12), @sizeOf(tgt.Cfg));
    try std.testing.expectEqual(@as(usize, 8), @offsetOf(tgt.Cfg, "mode"));
    try std.testing.expectEqual(@as(usize, 9), @offsetOf(tgt.Cfg, "lsb_first"));
}

test "spcmd maps the SPI mode, LSB-first and 8-bit frames" {
    try std.testing.expectEqual(@as(u32, 0x70000), tgt.spcmd(&cfg(0, false)));
    try std.testing.expectEqual(@as(u32, 0x70001), tgt.spcmd(&cfg(1, false)));
    try std.testing.expectEqual(@as(u32, 0x70002), tgt.spcmd(&cfg(2, false)));
    try std.testing.expectEqual(@as(u32, 0x71003), tgt.spcmd(&cfg(3, true)));
    try std.testing.expectEqual(@as(u32, 0x70000), tgt.spcmd(&cfg(9, false)));
}

test "mstpId picks MSTPB19 for SPI0 and MSTPB18 for SPI1" {
    try std.testing.expectEqual(@as(u16, 0x113), tgt.mstpId(0));
    try std.testing.expectEqual(@as(u16, 0x112), tgt.mstpId(1));
}

test "init checks cfg before the channel and programs target mode" {
    var r = Regs{};
    var c = Svc{};
    const k = cfg(3, true);
    try std.testing.expectEqual(tgt.err_null_ptr, tgt.init(&r, &c, 7, null));
    try std.testing.expectEqualStrings("target_init: cfg", std.mem.span(c.last));
    try std.testing.expectEqual(tgt.err_invalid_arg, tgt.init(&r, &c, 2, &k));
    try std.testing.expectEqual(tgt.ok, tgt.init(&r, &c, 1, &k));
    try std.testing.expectEqual(@as(u16, 0x112), c.mstp_id);
    try std.testing.expectEqual(tgt.spcr_target, r.at(1, tgt.off_spcr));
    try std.testing.expectEqual(@as(u32, 0x71003), r.at(1, tgt.off_spcmd0));
    try std.testing.expectEqual(@as(u32, 1), r.at(1, tgt.off_spfcr));
    try std.testing.expectEqual(tgt.spsrc_all, r.at(1, tgt.off_spsrc));
    try std.testing.expectEqual(@as(u32, 1), c.info_val);
}

test "init hands up an mstp failure before touching registers" {
    var r = Regs{};
    var c = Svc{ .mstp_err = 0x201 };
    const k = cfg(0, false);
    try std.testing.expectEqual(@as(u16, 0x201), tgt.init(&r, &c, 0, &k));
    try std.testing.expectEqual(@as(u32, 0), r.writes);
    try std.testing.expectEqualStrings("target_init: mstp", std.mem.span(c.last));
}

test "xfer exchanges one byte and clears both flags" {
    var r = Regs{ .busy = 3 };
    var c = Svc{};
    var rx: u8 = 0;
    try std.testing.expectEqual(tgt.ok, tgt.xfer(&r, &c, 0, 0x5A, &rx));
    try std.testing.expectEqual(@as(u8, 0xA5), rx);
    try std.testing.expectEqual(tgt.spsr_sprf, r.at(0, tgt.off_spsrc));
    try std.testing.expectEqual(@as(u32, 0x5A), r.at(0, tgt.off_spdr));
    try std.testing.expectEqual(tgt.ok, tgt.xfer(&r, &c, 0, 1, null));
}

test "xfer refuses a bad channel and times out on a stuck flag" {
    var r = Regs{ .ready = tgt.spsr_sptef };
    var c = Svc{};
    try std.testing.expectEqual(tgt.err_null_ptr, tgt.xfer(&r, &c, 2, 0, null));
    try std.testing.expectEqualStrings("target_xfer: channel out of range", std.mem.span(c.last));
    try std.testing.expectEqual(tgt.err_hw_timeout, tgt.xfer(&r, &c, 1, 0, null));
    r.ready = 0;
    try std.testing.expectEqual(tgt.err_hw_timeout, tgt.xfer(&r, &c, 1, 0, null));
}
