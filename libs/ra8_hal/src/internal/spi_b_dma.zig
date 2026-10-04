//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! SPI_B byte-wide DMA request building (HUM Ch 43). Pure helpers; the
//! C ABI and the cache/DMA calls live in spi_b_dma_abi.zig (RA8FW-576).

/// HUM Ch 43: SPI0 at 0x4035C000, SPI1 one 0x100 stride above.
pub const spi0_base: usize = 0x4035C000;
pub const channel_stride: usize = 0x100;
pub const channel_count: u8 = 2;
/// HUM Ch 43.2.2 "SPDR : SPI Data Register" p 2881, offset +0x00.
pub const off_spdr: usize = 0x00;

/// k_ra8_dmac_width_byte (DMTMD.SZ = 00b).
pub const width_byte: u8 = 0;

pub const CompleteFn = *const fn (?*anyopaque) callconv(.c) void;

/// Mirror of ra8_dma_request_t (ra8_dma.h).
pub const Request = extern struct {
    src_addr: usize,
    dst_addr: usize,
    count: u16,
    width: u8,
    src_inc: bool,
    dst_inc: bool,
    trigger: u8,
    on_complete: ?CompleteFn,
    ctx: ?*anyopaque,
};

pub fn argsOk(channel: u8, len: u16) bool {
    return channel < channel_count and len != 0;
}

pub fn spdrAddr(channel: u8) usize {
    return spi0_base + @as(usize, channel) * channel_stride + off_spdr;
}

/// Round up to a whole number of cache lines; line must be a power of two.
pub fn roundUp(bytes: u32, line: u32) u32 {
    if (bytes == 0 or line == 0) return bytes;
    return (bytes + (line - 1)) & ~(line - 1);
}

/// Memory -> SPDR, source increments.
pub fn txRequest(channel: u8, src: usize, len: u16, cb: ?CompleteFn, ctx: ?*anyopaque) Request {
    return .{ .src_addr = src, .dst_addr = spdrAddr(channel), .count = len, .width = width_byte, .src_inc = true, .dst_inc = false, .trigger = 0, .on_complete = cb, .ctx = ctx };
}

/// SPDR -> memory, destination increments.
pub fn rxRequest(channel: u8, dst: usize, len: u16, cb: ?CompleteFn, ctx: ?*anyopaque) Request {
    return .{ .src_addr = spdrAddr(channel), .dst_addr = dst, .count = len, .width = width_byte, .src_inc = false, .dst_inc = true, .trigger = 0, .on_complete = cb, .ctx = ctx };
}
