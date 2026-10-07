//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for ra8_spi_init, ra8_spi_deinit and ra8_spi_controller_init
//! (RA8FW-898, was part of ra8_spi_b.c). HUM Ch 43.2 p 2881-2906,
//! Ch 11.2.7 "MSTPCRB" p 444.

const common = @import("abi_common.zig");
const clock = @import("internal/spi_b_clock.zig");
const events = @import("internal/spi_b_events.zig");
const setup = @import("internal/spi_b_setup.zig");

extern fn ra8_mstp_enable(id: u16) u16;
extern fn ra8_mstp_disable(id: u16) u16;
extern fn priv_ra8_spi_b_spbr(baud_hz: u32, pclka_hz: u32) u8;
/// Owned by src/spi_b_events_abi.zig (RA8FW-894).
extern var s_spi_state: [channel_count]events.State;

const tag = "SPI_B";
const channel_count: u8 = 2;
const bases = [channel_count]usize{ 0x4035_C000, 0x4035_C100 };
const mstp_ids = [channel_count]u16{ (1 << 8) | 19, (1 << 8) | 18 }; // MSTPB19 SPI0, MSTPB18 SPI1

const Reg = enum(usize) {
    spdecr = 0x04,
    spcr = 0x08,
    spcr2 = 0x0C,
    spcr3 = 0x10,
    spcmd0 = 0x14,
    spdcr = 0x40,
    spdcr2 = 0x44,
    spsrc = 0x68,
    spfcr = 0x6C,
};

fn put(channel: u8, r: Reg, value: u32) void {
    const p: *volatile u32 = @ptrFromInt(bases[channel] + @backingInt(r));
    p.* = value;
}

/// The SPE=0 program sequence; SPCR2 only honours writes while SPE is clear.
fn program(channel: u8, cfg: setup.Cfg) void {
    put(channel, .spsrc, setup.spsrc_all);
    const spbr = priv_ra8_spi_b_spbr(cfg.baud_hz, cfg.pclka_hz);
    put(channel, .spcr3, clock.withSpbr(0, spbr));
    put(channel, .spdecr, 0);
    put(channel, .spcr2, setup.spcr2(cfg));
    put(channel, .spcmd0, setup.spcmd(cfg));
    put(channel, .spdcr, 0);
    put(channel, .spdcr2, 0);
    put(channel, .spfcr, setup.spfcr_spfrst);
    // SPFRST can leave SPRF set with a stale byte; re-clear before SPE.
    put(channel, .spsrc, setup.spsrc_all);
}

fn resetState(channel: u8, initialized: bool) void {
    s_spi_state[channel] = .{ .cb = null, .ctx = null, .initialized = initialized };
}

export fn ra8_spi_init(channel: u8, cfg: ?*const setup.Cfg) u16 {
    const c = cfg orelse {
        common.ra8_log_emit_error(tag, "spi_init: cfg");
        return common.k_ra8_err_null_ptr;
    };
    if (channel >= channel_count) return common.k_ra8_err_invalid_arg;
    const mst = ra8_mstp_enable(mstp_ids[channel]);
    if (mst != common.k_ra8_ok) {
        common.ra8_log_emit_error(tag, "spi_init: mstp");
        common.ra8_log_emit_error_val(tag, "Error", mst);
        return mst;
    }
    put(channel, .spcr, 0);
    program(channel, c.*);
    put(channel, .spcr, setup.spcrController());
    resetState(channel, true);
    common.ra8_log_emit_info_val(tag, "spi_init channel", channel);
    return common.k_ra8_ok;
}

export fn ra8_spi_deinit(channel: u8) u16 {
    if (channel >= channel_count) return common.k_ra8_err_invalid_arg;
    put(channel, .spcr, 0);
    resetState(channel, false);
    return ra8_mstp_disable(mstp_ids[channel]);
}

/// Out-of-range is null_ptr here, not invalid_arg: the existing test contract.
export fn ra8_spi_controller_init(channel: u8) u16 {
    if (channel >= channel_count) return common.k_ra8_err_null_ptr;
    return ra8_spi_init(channel, &setup.default_cfg);
}
