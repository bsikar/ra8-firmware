//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! SD card protocol logic (RA8FW-791): CSD decode, classification,
//! addressing and the command sequences against a scripted host.

const std = @import("std");
const sd = @import("sdcard");

const Fake = struct {
    cmds: [8]u32 = @splat(0),
    args: [8]u32 = @splat(0),
    sends: usize = 0,
    fail_cmd: u32 = 0xFF,
    echo: u32 = 0x1AA,
    ready_after: usize = 1,
    rounds: usize = 0,
    csd: [4]u32 = .{ 0, 0x1D_0000, 0, 0x4000_0000 },
    logged: ?[*:0]const u8 = null,

    pub fn send(self: *Fake, cmd: u32, arg: u32, rsp: *[4]u32) u16 {
        if (self.sends < 8) {
            self.cmds[self.sends] = cmd;
            self.args[self.sends] = arg;
        }
        self.sends += 1;
        if (cmd == self.fail_cmd) return 0x203;
        switch (cmd) {
            sd.cmd8_send_if_cond => rsp[0] = self.echo,
            sd.acmd41_op_cond => {
                self.rounds += 1;
                rsp[0] = if (self.rounds >= self.ready_after) 0xC0FF_8000 else 0x00FF_8000;
            },
            sd.cmd3_send_rca => rsp[0] = 0xB368_0500,
            sd.cmd9_send_csd => rsp.* = self.csd,
            else => {},
        }
        return sd.ok;
    }
    pub fn failed(self: *Fake, err: u16, msg: [*:0]const u8) bool {
        if (err == sd.ok) return false;
        self.logged = msg;
        return true;
    }
};

test "CSD v2 counts 512 KiB units" {
    var blocks: u32 = 0;
    const rsp = [4]u32{ 0, 0x1D_0000 << 0, 0, 0x4000_0000 };
    try std.testing.expectEqual(sd.ok, sd.decodeCsd(&rsp, &blocks));
    try std.testing.expectEqual(@as(u32, (0x1D + 1) * 1024), blocks);
    const big = [4]u32{ 0, 0xEDC8_0000, 0x0000_0001, 0x4000_0000 };
    try std.testing.expectEqual(sd.ok, sd.decodeCsd(&big, &blocks));
    try std.testing.expectEqual(@as(u32, (0x1_EDC8 + 1) * 1024), blocks);
}

test "CSD v1 converts to 512-byte sectors; other structures fail" {
    // READ_BL_LEN 10, C_SIZE 0xFFF, C_SIZE_MULT 7: 4096 * 512 * 1024 / 512.
    const rsp = [4]u32{ 0, (0x3FF << 22) | (7 << 7), (10 << 16) | 0x3, 0 };
    var blocks: u32 = 0;
    try std.testing.expectEqual(sd.ok, sd.decodeCsd(&rsp, &blocks));
    try std.testing.expectEqual(@as(u32, 4096 * 512 * 2), blocks);
    const bad = [4]u32{ 0, 0, 0, 0x8000_0000 };
    blocks = 7;
    try std.testing.expectEqual(sd.err_hw_init_failed, sd.decodeCsd(&bad, &blocks));
    try std.testing.expectEqual(@as(u32, 7), blocks);
}

test "classification and card addressing" {
    try std.testing.expectEqual(sd.type_sdsc, sd.classify(false, 1 << 30));
    try std.testing.expectEqual(sd.type_sdhc, sd.classify(true, 67_108_864));
    try std.testing.expectEqual(sd.type_sdxc, sd.classify(true, 67_108_865));
    try std.testing.expectEqual(@as(u32, 5 * 512), sd.cardAddress(sd.type_sdsc, 5));
    try std.testing.expectEqual(@as(u32, 5), sd.cardAddress(sd.type_sdhc, 5));
}

test "identify sends CMD0 then CMD8 and checks the echo" {
    var host = Fake{};
    try std.testing.expectEqual(sd.ok, sd.identify(&host));
    try std.testing.expectEqualSlices(u32, &.{ 0, 8 }, host.cmds[0..2]);
    try std.testing.expectEqual(@as(u32, 0x1AA), host.args[1]);
    host = Fake{ .echo = 0x1AB };
    try std.testing.expectEqual(sd.err_hw_init_failed, sd.identify(&host));
    host = Fake{ .fail_cmd = 0 };
    try std.testing.expectEqual(@as(u16, 0x203), sd.identify(&host));
    try std.testing.expectEqualStrings("cmd0", std.mem.span(host.logged.?));
}

test "ACMD41 repeats CMD55 + ACMD41 until busy clears" {
    var host = Fake{ .ready_after = 3 };
    var ocr: u32 = 0;
    try std.testing.expectEqual(sd.ok, sd.acmd41(&host, &ocr));
    try std.testing.expectEqual(@as(u32, 0xC0FF_8000), ocr);
    try std.testing.expectEqual(@as(usize, 6), host.sends);
    try std.testing.expectEqualSlices(u32, &.{ 55, 41, 55, 41 }, host.cmds[0..4]);
    try std.testing.expectEqual(sd.acmd41_arg, host.args[1]);
}

test "ACMD41 gives up after 1000 rounds" {
    var host = Fake{ .ready_after = 5000 };
    var ocr: u32 = 0x55;
    try std.testing.expectEqual(sd.err_hw_init_failed, sd.acmd41(&host, &ocr));
    try std.testing.expectEqual(@as(usize, 2000), host.sends);
    try std.testing.expectEqual(@as(u32, 0x55), ocr);
}

test "publish and select: CMD2, CMD3, CMD9 and CMD7 with RCA<<16" {
    var host = Fake{};
    var rca: u16 = 0;
    var blocks: u32 = 0;
    try std.testing.expectEqual(sd.ok, sd.publishAndSelect(&host, &rca, &blocks));
    try std.testing.expectEqual(@as(u16, 0xB368), rca);
    try std.testing.expectEqual(@as(u32, 30 * 1024), blocks);
    try std.testing.expectEqualSlices(u32, &.{ 2, 3, 9, 7 }, host.cmds[0..4]);
    try std.testing.expectEqual(@as(u32, 0xB368_0000), host.args[2]);
    try std.testing.expectEqual(@as(u32, 0xB368_0000), host.args[3]);
    host = Fake{ .fail_cmd = 7 };
    rca = 0;
    try std.testing.expectEqual(@as(u16, 0x203), sd.publishAndSelect(&host, &rca, &blocks));
    try std.testing.expectEqual(@as(u16, 0), rca);
    try std.testing.expectEqualStrings("cmd7", std.mem.span(host.logged.?));
}
