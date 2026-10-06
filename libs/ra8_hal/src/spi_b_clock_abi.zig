//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for ra8_spi_set_clock, ra8_spi_get_errors, ra8_spi_clear_errors and
//! the shared SPBR divider (RA8FW-892, was part of ra8_spi_b.c). Same check
//! order and error codes as the C. HUM Ch 43.2.13 "SPSRC" p 2905.

const common = @import("abi_common.zig");
const clock = @import("internal/spi_b_clock.zig");

const tag = "SPI_B";
const channel_count: u8 = 2;
const bases = [channel_count]usize{ 0x4035_C000, 0x4035_C100 };

fn reg(channel: u8, off: usize) *volatile u32 {
    return @ptrFromInt(bases[channel] + off);
}
const spcr3_off = 0x10;
const spsr_off = 0x50;
const spsrc_off = 0x68;

/// Shared with ra8_spi_init in src/spi_b_setup_abi.zig.
export fn priv_ra8_spi_b_spbr(baud_hz: u32, pclka_hz: u32) u8 {
    return clock.spbr(baud_hz, pclka_hz);
}

export fn ra8_spi_set_clock(channel: u8, baud_hz: u32, pclka_hz: u32) u16 {
    if (channel >= channel_count) return common.k_ra8_err_invalid_arg;
    if (baud_hz == 0) return common.k_ra8_err_invalid_arg;
    const spcr3 = reg(channel, spcr3_off);
    spcr3.* = clock.withSpbr(spcr3.*, clock.spbr(baud_hz, pclka_hz));
    return common.k_ra8_ok;
}

export fn ra8_spi_get_errors(channel: u8, out_mask: ?*u8) u16 {
    const out = out_mask orelse {
        common.ra8_log_emit_error(tag, "spi get_errors");
        return common.k_ra8_err_null_ptr;
    };
    if (channel >= channel_count) return common.k_ra8_err_invalid_arg;
    out.* = clock.errMask(reg(channel, spsr_off).*);
    return common.k_ra8_ok;
}

/// SPI_B status flags clear through SPSRC (write 1).
export fn ra8_spi_clear_errors(channel: u8) u16 {
    if (channel >= channel_count) return common.k_ra8_err_invalid_arg;
    reg(channel, spsrc_off).* = clock.spsr_errs;
    return common.k_ra8_ok;
}
