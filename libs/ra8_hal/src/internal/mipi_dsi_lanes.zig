//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! MIPI DSI-2 lane and clock controls (RA8FW-643): soft reset, HS clock
//! start/stop, ULPS enter/exit and the bounded poll helper, ported from
//! ra8_mipi_dsi.c. Registers go through a `dsi` ops value (offsets from
//! the DSI base) and the lane-state flags through `Flags`, so host tests
//! can use a fake register file.

pub const ok: u16 = 0;
pub const invalid_arg: u16 = 0x103;
pub const hw_timeout: u16 = 0x203;

pub const off_hsclksetr: u16 = 0x104;
pub const off_ulpscr: u16 = 0x10C;
pub const off_rstcr: u16 = 0x110;
pub const off_plsr: u16 = 0x320;

pub const rstcr_swrst: u32 = 1 << 0;
pub const hsclk_start: u32 = 1 << 0;
pub const hsclk_continuous: u32 = 1 << 1;
pub const ulpscr_clent: u32 = 1 << 24;
pub const ulpscr_clexit: u32 = 1 << 25;
pub const ulpscr_dlent: u32 = 1 << 28;
pub const ulpscr_dlexit: u32 = 1 << 29;
pub const plsr_cllp2hs: u32 = 1 << 26;
pub const plsr_clhs2lp: u32 = 1 << 27;

pub const lane_none: u8 = 0;
pub const lane_clock: u8 = 1 << 0;
pub const lane_data: u8 = 1 << 1;
pub const busy_loop_max: u32 = 100_000;

/// The driver's software lane state (owned by the C-ABI layer).
pub const Flags = struct {
    continuous_clock: *bool,
    clock_ulps: *bool,
    data_ulps: *bool,
};

/// Polls `src.read()` until `(value & mask) == expect` or the cap runs out.
pub fn waitEq(src: anytype, mask: u32, expect: u32) u16 {
    var i: u32 = 0;
    while (i < busy_loop_max) : (i += 1) {
        if (src.read() & mask == expect) return ok;
    }
    return hw_timeout;
}

fn RegReader(comptime Dsi: type) type {
    return struct {
        dsi: Dsi,
        off: u16,
        pub fn read(r: @This()) u32 {
            return r.dsi.read32(r.off);
        }
    };
}

fn waitPlsr(dsi: anytype, bit: u32) u16 {
    const reader = RegReader(@TypeOf(dsi)){ .dsi = dsi, .off = off_plsr };
    return waitEq(reader, bit, bit);
}

pub fn softReset(dsi: anytype) u16 {
    dsi.write32(off_rstcr, rstcr_swrst);
    dsi.write32(off_rstcr, 0);
    return ok;
}

pub fn hsClockStart(dsi: anytype, flags: Flags) u16 {
    var hsclk = hsclk_start;
    if (flags.continuous_clock.*) hsclk |= hsclk_continuous;
    dsi.write32(off_hsclksetr, hsclk);
    return waitPlsr(dsi, plsr_cllp2hs);
}

pub fn hsClockStop(dsi: anytype) u16 {
    dsi.write32(off_hsclksetr, 0);
    return waitPlsr(dsi, plsr_clhs2lp);
}

pub fn ulpsEnter(dsi: anytype, flags: Flags, lanes: u8) u16 {
    if (lanes == lane_none) return invalid_arg;
    if (lanes & lane_clock != 0 and flags.continuous_clock.*) {
        dsi.errMsg("ulps_enter: clock lane + continuous mode rejected");
        return invalid_arg;
    }
    var ulpscr: u32 = 0;
    if (lanes & lane_data != 0 and !flags.data_ulps.*) {
        ulpscr |= ulpscr_dlent;
        flags.data_ulps.* = true;
    }
    if (lanes & lane_clock != 0 and !flags.clock_ulps.*) {
        ulpscr |= ulpscr_clent;
        flags.clock_ulps.* = true;
    }
    dsi.write32(off_ulpscr, ulpscr);
    return ok;
}

pub fn ulpsExit(dsi: anytype, flags: Flags, lanes: u8) u16 {
    if (lanes == lane_none) return invalid_arg;
    var ulpscr: u32 = 0;
    if (lanes & lane_data != 0 and flags.data_ulps.*) {
        ulpscr |= ulpscr_dlexit;
        flags.data_ulps.* = false;
    }
    if (lanes & lane_clock != 0 and flags.clock_ulps.*) {
        ulpscr |= ulpscr_clexit;
        flags.clock_ulps.* = false;
    }
    dsi.write32(off_ulpscr, ulpscr);
    return ok;
}
