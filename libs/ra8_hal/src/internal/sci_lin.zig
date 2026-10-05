//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Simple LIN on SCI (RA8FW-754), the logic half of ra8_sci_lin.h: PID
//! parity, the classic and enhanced checksums, header and response checks,
//! cfg validation and the mode programming sequence. Register access goes
//! through a `hw` value so tests can record it (HUM 38.2.14 to 38.2.28).

pub const base: usize = 0x40358000;
pub const stride: usize = 0x100;
pub const channel_max: u8 = 9;

pub const off_ccr0: usize = 0x08;
pub const off_ccr3: usize = 0x14;
pub const off_xcr0: usize = 0x34;
pub const off_xcr1: usize = 0x38;
pub const off_xcr2: usize = 0x3C;
pub const off_xsr0: usize = 0x5C;
pub const off_xfclr: usize = 0x78;

pub const ccr3_mask_mod: u32 = 0x0007_0000;
pub const ccr3_mod_simple_lin: u32 = 0x6 << 16;
pub const xcr0_bfe: u32 = 1 << 8;
pub const xcr1_tcst: u32 = 1 << 0;
pub const xcr1_sdst: u32 = 1 << 4;
pub const xcr1_bmen: u32 = 1 << 5;
pub const xcr2_shift_bflw: u5 = 16;
pub const xcr2_bflw_max: u16 = 0xFFFE;
pub const xsr0_bfdf: u32 = 1 << 10;
pub const xfclr_default: u32 = 0x0000_FF00;
pub const ccr0_te_re: u32 = (1 << 4) | (1 << 0);

pub const sync_byte: u8 = 0x55;
pub const id_max: u8 = 0x3F;
pub const data_max: u8 = 8;

pub const role_commander: u8 = 0;
pub const role_responder: u8 = 1;
pub const clk_div4: u8 = 1;
pub const clk_div64: u8 = 3;
pub const checksum_classic: u8 = 0;
pub const checksum_enhanced: u8 = 1;

/// ra8_sci_cfg_t: baud, data bits, parity, stop bits, PCLK (12 bytes).
pub const UartCfg = extern struct {
    baud: u32,
    data_bits: u8,
    parity: u8,
    stop_bits: u8,
    pclk_hz: u32,
};

/// ra8_sci_lin_cfg_t (16 bytes).
pub const Cfg = extern struct {
    uart: UartCfg,
    role: u8,
    timer_clk: u8,
    break_field_len: u16,
};

comptime {
    if (@sizeOf(UartCfg) != 12 or @offsetOf(UartCfg, "pclk_hz") != 8) @compileError("ra8_sci_cfg_t layout");
    if (@sizeOf(Cfg) != 16 or @offsetOf(Cfg, "role") != 12) @compileError("ra8_sci_lin_cfg_t layout");
    if (@offsetOf(Cfg, "timer_clk") != 13 or @offsetOf(Cfg, "break_field_len") != 14) @compileError("ra8_sci_lin_cfg_t layout");
}

pub fn channelOk(channel: u8) bool {
    return channel <= channel_max;
}

pub fn regAddr(channel: u8, offset: usize) usize {
    return base + @as(usize, channel) * stride + offset;
}

/// Role, break length (0xFFFF is prohibited) and TCSS must be legal.
pub fn cfgOk(cfg: Cfg) bool {
    if (cfg.role > role_responder) return false;
    if (cfg.break_field_len > xcr2_bflw_max) return false;
    return cfg.timer_clk >= clk_div4 and cfg.timer_clk <= clk_div64;
}

/// Protected id: id[5:0] plus P0 = id0^id1^id2^id4 and P1 = !(id1^id3^id4^id5).
pub fn pid(id: u8) u8 {
    const v = id & id_max;
    const b = struct {
        fn at(x: u8, n: u3) u8 {
            return (x >> n) & 1;
        }
    }.at;
    const p0 = b(v, 0) ^ b(v, 1) ^ b(v, 2) ^ b(v, 4);
    const p1 = (b(v, 1) ^ b(v, 3) ^ b(v, 4) ^ b(v, 5)) ^ 1;
    return v | (p0 << 6) | (p1 << 7);
}

/// Two carry folds of the 16-bit sum, then the one's complement low byte.
pub fn foldComplement(start: u16) u8 {
    var sum = start;
    for (0..2) |_| sum = (sum & 0xFF) +% (sum >> 8);
    return @truncate(~sum & 0xFF);
}

/// Enhanced mode seeds the sum with the PID; classic sums the data only.
pub fn checksum(mode: u8, p: u8, data: []const u8) u8 {
    var sum: u16 = if (mode == checksum_enhanced) p else 0;
    for (data) |byte| sum +%= byte;
    return foldComplement(sum);
}

pub const Header = struct { id: u8, valid: bool };

pub fn checkHeader(sync: u8, p: u8) Header {
    const id = p & id_max;
    return .{ .id = id, .valid = sync == sync_byte and pid(id) == p };
}

/// CCR0 off, CCR3.MOD = simple LIN, XCR0 TCSS|BFE, XCR2 BFLW, XCR1 start
/// frame detect + bit rate measure for a responder, then TE|RE.
pub fn programMode(hw: anytype, channel: u8, role: u8, tcss: u32, break_len: u16) void {
    hw.write32(regAddr(channel, off_ccr0), 0);
    const ccr3 = hw.read32(regAddr(channel, off_ccr3));
    hw.write32(regAddr(channel, off_ccr3), (ccr3 & ~ccr3_mask_mod) | ccr3_mod_simple_lin);
    hw.write32(regAddr(channel, off_xcr0), tcss | xcr0_bfe);
    hw.write32(regAddr(channel, off_xcr2), @as(u32, break_len) << xcr2_shift_bflw);
    const xcr1: u32 = if (role == role_responder) xcr1_sdst | xcr1_bmen else 0;
    hw.write32(regAddr(channel, off_xcr1), xcr1);
    hw.write32(regAddr(channel, off_ccr0), ccr0_te_re);
}
