//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for ra8_spi_b_target_init / ra8_spi_b_target_xfer (RA8FW-596).
//! The register sequences are in internal/spi_b_target.zig.

const common = @import("abi_common.zig");
const tgt = @import("internal/spi_b_target.zig");

const tag = "SPI_B_TGT";

extern fn ra8_mstp_enable(id: u16) u16;

const Mmio = struct {
    fn reg(ch: u8, off: usize) *volatile u32 {
        return @ptrFromInt(tgt.base0 + @as(usize, ch) * tgt.stride + off);
    }
    pub fn read32(_: Mmio, ch: u8, off: usize) u32 {
        return reg(ch, off).*;
    }
    pub fn write32(_: Mmio, ch: u8, off: usize, value: u32) void {
        reg(ch, off).* = value;
    }
};

const C = struct {
    pub fn mstpEnable(_: C, id: u16) u16 {
        return ra8_mstp_enable(id);
    }
    pub fn infoVal(_: C, msg: [*:0]const u8, value: u32) void {
        common.ra8_log_emit_info_val(tag, msg, value);
    }
    pub fn err(_: C, msg: [*:0]const u8) void {
        common.ra8_log_emit_error(tag, msg);
    }
    pub fn fail(_: C, msg: [*:0]const u8, code: u16) void {
        common.ra8_log_emit_error(tag, msg);
        common.ra8_log_emit_error_val(tag, "Error", code);
    }
};

export fn ra8_spi_b_target_init(channel: u8, cfg: ?*const tgt.Cfg) u16 {
    return tgt.init(Mmio{}, C{}, channel, cfg);
}

export fn ra8_spi_b_target_xfer(channel: u8, tx: u8, rx: ?*u8) u16 {
    return tgt.xfer(Mmio{}, C{}, channel, tx, rx);
}
