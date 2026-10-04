//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const dsi = @import("mipi_dsi_cmd");

const Fake = struct {
    regs: [0x810 / 4]u32 = [_]u32{0} ** (0x810 / 4),
    msgs: u8 = 0,
    vals: u8 = 0,
    pending: ?[*]u8 = null,
    pending_len: u16 = 0,

    pub fn read32(f: *Fake, off: u16) u32 {
        return f.regs[off / 4];
    }
    pub fn write32(f: *Fake, off: u16, value: u32) void {
        f.regs[off / 4] = value;
    }
    pub fn errMsg(f: *Fake, _: [*:0]const u8) void {
        f.msgs += 1;
    }
    pub fn errVal(f: *Fake, _: [*:0]const u8, _: u16) void {
        f.vals += 1;
    }
    pub fn setPendingRx(f: *Fake, buf: [*]u8, len: u16) void {
        f.pending = buf;
        f.pending_len = len;
    }
    fn at(f: *const Fake, off: u16) u32 {
        return f.regs[off / 4];
    }
};

fn base(cmd_id: u8) dsi.Command {
    return .{
        .cmd_id = cmd_id,
        .virtual_channel = 0,
        .bta = dsi.bta_none,
        .low_power = true,
        .ack_request = false,
        .aux_operation = false,
        .action_code = 0,
        .tx_len = 0,
        .p_tx_buffer = null,
        .p_rx_buffer = null,
    };
}

test "short packet goes out on channel 0 with both params in descriptor A" {
    var f = Fake{};
    try std.testing.expectEqual(dsi.ok, dsi.sendShortPacket(&f, 0x15, 1, 0x36, 0x48));
    const a = f.at(dsi.off_sqch0dsc);
    try std.testing.expectEqual(@as(u32, 0x36 | 0x48 << 8 | 0x15 << 16 | 1 << 22 | 1 << 25), a);
    try std.testing.expectEqual(dsi.dsc_b_dtsel_seqrm, f.at(dsi.off_sqch0dsc + 4));
    try std.testing.expectEqual(dsi.dsc_c_finact, f.at(dsi.off_sqch0dsc + 8));
    try std.testing.expectEqual(dsi.sqch_chsel | dsi.sqch_start, f.at(dsi.off_sqch0set0r));
    try std.testing.expectEqual(dsi.sqch_chsel, f.at(dsi.off_sqch1set0r));
    try std.testing.expectEqual(@as(?[*]u8, null), f.pending);
}

test "long HS packet stages payload, sets FMT and uses channel 1" {
    var f = Fake{};
    var data: [18]u8 = undefined;
    for (&data, 0..) |*b, i| b.* = @intCast(i + 1);
    try std.testing.expectEqual(dsi.ok, dsi.sendLongPacket(&f, 0x39, 0, &data, 18, false));
    try std.testing.expectEqual(@as(u32, 0x04030201), f.at(dsi.off_txppd0r));
    try std.testing.expectEqual(@as(u32, 0x100F0E0D), f.at(dsi.off_txppd0r + 12));
    const a = f.at(dsi.off_sqch1dsc);
    try std.testing.expectEqual(@as(u32, 18 | 0x39 << 16 | 1 << 24), a);
    try std.testing.expectEqual(@as(u32, @truncate(@intFromPtr(&data))), f.at(dsi.off_sqch1dsc + 12));
    try std.testing.expectEqual(dsi.sqch_chsel, f.at(dsi.off_sqch0set0r));
    try std.testing.expectEqual(dsi.sqch_chsel | dsi.sqch_start, f.at(dsi.off_sqch1set0r));
}

test "validate rejects bad VC, missing buffer and oversize payloads" {
    var c = base(0x39);
    c.virtual_channel = 4;
    try std.testing.expectEqual(dsi.invalid_arg, dsi.validate(&c));
    c = base(0x39);
    c.tx_len = 1;
    try std.testing.expectEqual(dsi.null_ptr, dsi.validate(&c));
    var buf = [_]u8{0} ** 4;
    c.p_tx_buffer = &buf;
    c.tx_len = 129;
    try std.testing.expectEqual(dsi.invalid_arg, dsi.validate(&c));
    c.low_power = false;
    try std.testing.expectEqual(dsi.ok, dsi.validate(&c));
    c.tx_len = 1025;
    try std.testing.expectEqual(dsi.invalid_arg, dsi.validate(&c));
}

test "send_command logs and refuses a null command or a bad one" {
    var f = Fake{};
    try std.testing.expectEqual(dsi.null_ptr, dsi.sendCommand(&f, null));
    try std.testing.expectEqual(@as(u8, 1), f.msgs);
    var c = base(0x05);
    c.virtual_channel = 7;
    try std.testing.expectEqual(dsi.invalid_arg, dsi.sendCommand(&f, &c));
    try std.testing.expectEqual(@as(u8, 1), f.vals);
    try std.testing.expectEqual(@as(u32, 0), f.at(dsi.off_sqch0set0r));
}

test "link state blocks LP during video and any running sequence" {
    var f = Fake{};
    var c = base(0x05);
    f.regs[dsi.off_linksr / 4] = dsi.link_vrun;
    try std.testing.expectEqual(dsi.invalid_state, dsi.sendCommand(&f, &c));
    c.low_power = false;
    try std.testing.expectEqual(dsi.ok, dsi.sendCommand(&f, &c));
    c.aux_operation = true;
    try std.testing.expectEqual(dsi.invalid_state, dsi.sendCommand(&f, &c));
    f.regs[dsi.off_linksr / 4] = dsi.link_sq1run;
    try std.testing.expectEqual(dsi.busy, dsi.sendCommand(&f, &c));
    try std.testing.expectEqual(@as(u8, 2), f.msgs);
    try std.testing.expectEqual(@as(u8, 3), f.vals);
}

test "aux operation packs ACTCODE into descriptor C" {
    var c = base(0x05);
    c.aux_operation = true;
    c.action_code = 0xA5;
    try std.testing.expectEqual(dsi.dsc_c_finact | dsi.dsc_c_auxop | 0xA500_0000, dsi.makeDscC(&c));
    c.bta = 3;
    c.cmd_id = 0x7F;
    try std.testing.expectEqual(@as(u32, 0x3F << 16 | 1 << 24 | 1 << 25 | 3 << 26), dsi.makeDscA(&c));
}

test "short packet descriptor A takes one byte when tx_len is 1" {
    var c = base(0x05);
    const buf = [_]u8{ 0x11, 0x22 };
    c.p_tx_buffer = &buf;
    c.tx_len = 1;
    try std.testing.expectEqual(@as(u32, 0x11 | 0x05 << 16 | 1 << 25), dsi.makeDscA(&c));
    try std.testing.expect(!dsi.isLong(0x08) and dsi.isLong(0x09) and dsi.isLong(0x29));
}

test "read_packet arms pending RX with the caller length and points D at it" {
    var f = Fake{};
    var rx = [_]u8{0} ** 8;
    try std.testing.expectEqual(dsi.ok, dsi.readPacket(&f, 0x06, 0, 0x0A, 0, &rx, 8));
    try std.testing.expectEqual(@as(?[*]u8, &rx), f.pending);
    try std.testing.expectEqual(dsi.payload_max, f.pending_len);
    try std.testing.expectEqual(@as(u32, @truncate(@intFromPtr(&rx))), f.at(dsi.off_sqch0dsc + 12));
    try std.testing.expectEqual(@as(u32, 0x0A | 0x06 << 16 | 1 << 25 | 2 << 26), f.at(dsi.off_sqch0dsc));
}

test "read_packet rejects a null sink and a zero length" {
    var f = Fake{};
    var rx = [_]u8{0} ** 2;
    try std.testing.expectEqual(dsi.null_ptr, dsi.readPacket(&f, 0x06, 0, 0, 0, null, 2));
    try std.testing.expectEqual(@as(u8, 1), f.msgs);
    try std.testing.expectEqual(dsi.invalid_arg, dsi.readPacket(&f, 0x06, 0, 0, 0, &rx, 0));
    try std.testing.expectEqual(@as(?[*]u8, null), f.pending);
}

test "send_long_packet rejects data-less payloads before validating" {
    var f = Fake{};
    try std.testing.expectEqual(dsi.null_ptr, dsi.sendLongPacket(&f, 0x39, 0, null, 3, true));
    try std.testing.expectEqual(@as(u8, 0), f.vals);
    try std.testing.expectEqual(dsi.ok, dsi.sendLongPacket(&f, 0x39, 0, null, 0, true));
    try std.testing.expectEqual(@as(u32, 0), f.at(dsi.off_txppd0r));
}
