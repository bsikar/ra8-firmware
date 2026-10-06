//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the SPI_B polled transfers: ra8_spi_xfer8, ra8_spi_write,
//! ra8_spi_read and ra8_spi_write_read (RA8FW-899, was ra8_spi_b.c).
//! HUM Ch 43.2.2 "SPDR" p 2881, 43.2.9 "SPSR" p 2898, 43.2.13 "SPSRC" p 2905,
//! Ch 43.3.13 controller-mode operation p 2911.

const builtin = @import("builtin");
const common = @import("abi_common.zig");
const xfer = @import("internal/spi_b_xfer.zig");

/// Host builds link the C fake-MMIO wait seam (ra8_hw_err.h) so the C
/// suites can hold or time out SPSR. Freestanding builds never see it.
const hosted = builtin.os.tag != .freestanding;
const seam = struct {
    extern fn ra8_fake_mmio_wait_eval(reg: *const volatile anyopaque, iter: u32, real_cond: bool) bool;
};

const tag = "SPI_B";
const channel_count: u8 = 2;
const bases = [channel_count]usize{ 0x4035_C000, 0x4035_C100 };
const poll_limit: u32 = 200_000;
const spsr_sptef: u32 = 0x2000_0000;
const spsr_sprf: u32 = 0x8000_0000;

const Reg = enum(usize) { spdr = 0x00, spcmd0 = 0x14, spsr = 0x50, spsrc = 0x68 };

fn at(channel: u8, r: Reg) *volatile u32 {
    return @ptrFromInt(bases[channel] + @intFromEnum(r));
}

/// Bounded SPSR wait; 0x203 when the flag never shows.
fn waitSpsr(channel: u8, mask: u32) u16 {
    const reg = at(channel, .spsr);
    var i: u32 = 0;
    while (i < poll_limit) : (i += 1) {
        const cond = (reg.* & mask) != 0;
        if (if (hosted) seam.ra8_fake_mmio_wait_eval(reg, i, cond) else cond) return common.k_ra8_ok;
    }
    return common.k_ra8_err_hw_timeout;
}

/// One full-duplex frame: SPTEF, SPDR out, clear, SPRF, SPDR in, clear.
fn frame(channel: u8, out: u32) struct { rc: u16, in: u32 } {
    var rc = waitSpsr(channel, spsr_sptef);
    if (rc != common.k_ra8_ok) return .{ .rc = rc, .in = 0 };
    at(channel, .spdr).* = out;
    at(channel, .spsrc).* = spsr_sptef;
    rc = waitSpsr(channel, spsr_sprf);
    if (rc != common.k_ra8_ok) return .{ .rc = rc, .in = 0 };
    const in = at(channel, .spdr).*;
    at(channel, .spsrc).* = spsr_sprf;
    return .{ .rc = common.k_ra8_ok, .in = in };
}

export fn ra8_spi_xfer8(channel: u8, tx: u8, rx: ?*u8) u16 {
    if (channel >= channel_count) {
        common.ra8_log_emit_error(tag, "channel out of range");
        return common.k_ra8_err_null_ptr;
    }
    const r = frame(channel, tx);
    if (r.rc != common.k_ra8_ok) return r.rc;
    if (rx) |p| p.* = @truncate(r.in);
    return common.k_ra8_ok;
}

fn xferCommon(channel: u8, tx: ?[*]const u8, rx: ?[*]u8, len: u32, width: u8) u16 {
    if (channel >= channel_count) return common.k_ra8_err_invalid_arg;
    if (xfer.unitBytes(width) == null) return common.k_ra8_err_invalid_arg;
    if (len == 0) return common.k_ra8_ok;
    if (tx == null and rx == null) return common.k_ra8_err_null_ptr;
    const spcmd0 = at(channel, .spcmd0);
    spcmd0.* = xfer.withWidth(spcmd0.*, width);
    var i: u32 = 0;
    while (i < len) : (i += 1) {
        const out = if (tx) |t| xfer.load(t, i, width) else xfer.dummy(width);
        const r = frame(channel, out);
        if (r.rc != common.k_ra8_ok) return r.rc;
        if (rx) |p| xfer.store(p, i, width, r.in);
    }
    return common.k_ra8_ok;
}

export fn ra8_spi_write(channel: u8, tx: ?[*]const u8, len: u32, width: u8) u16 {
    if (tx == null and len > 0) return common.k_ra8_err_null_ptr;
    return xferCommon(channel, tx, null, len, width);
}

export fn ra8_spi_read(channel: u8, rx: ?[*]u8, len: u32, width: u8) u16 {
    if (rx == null and len > 0) return common.k_ra8_err_null_ptr;
    return xferCommon(channel, null, rx, len, width);
}

export fn ra8_spi_write_read(channel: u8, tx: ?[*]const u8, rx: ?[*]u8, len: u32, width: u8) u16 {
    if (len > 0 and (tx == null or rx == null)) return common.k_ra8_err_null_ptr;
    return xferCommon(channel, tx, rx, len, width);
}
