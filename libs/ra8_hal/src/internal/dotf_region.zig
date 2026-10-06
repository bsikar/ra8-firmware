//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! DOTF region staging (RA8FW-839, was part of ra8_dotf.c): validate a region
//! against its channel's XSPI window, refuse overlap with the other
//! channel's live region, arm a slot, and program the active one. Pure over
//! the state table and a register sink; the exports are in
//! src/dotf_region_abi.zig. HUM Ch 45.3.

const state = @import("dotf_state.zig");
/// Re-exported so host tests share the same Region and ChanState types.
pub const state_mod = state;
const power = @import("dotf_power.zig");

pub const ok: u16 = 0;
pub const invalid_arg: u16 = 0x103;
pub const invalid_state: u16 = 0x104;
pub const conflict: u16 = 0x408;

/// CONVAREAST/CONVAREAD hold only bits [31:12] (HUM 45.3.1/45.3.2).
pub const addr_mask: u32 = 0xFFFF_F000;

/// k_ra8_dotf{0,1}_window_{lo,hi}: DOTF0 pairs with XSPI0, DOTF1 with XSPI1.
const window_lo = [power.channel_count]u32{ 0x8000_0000, 0x7000_0000 };
const window_hi = [power.channel_count]u32{ 0x9FFF_FFFF, 0x7FFF_FFFF };

pub fn windowLo(channel: u8) u32 {
    return window_lo[channel];
}

pub fn windowHi(channel: u8) u32 {
    return window_hi[channel];
}

/// internal_validate_region: 4 KiB aligned ends, start <= end ("Setting
/// CONVAREAST[31:12] > CONVAREAED[31:12] is prohibited", HUM 45.3.1), a
/// real slot, and inside the channel's window (HUM 45.3 p 3049).
pub fn validate(channel: u8, r: *const state.Region) u16 {
    if (r.start_addr & power.addr_low_mask != 0) return invalid_arg;
    if (r.end_addr & power.addr_low_mask != 0) return invalid_arg;
    if (r.start_addr > r.end_addr) return invalid_arg;
    if (r.region_id >= state.max_regions) return invalid_arg;
    if (r.start_addr < windowLo(channel) or r.end_addr > windowHi(channel)) return invalid_arg;
    return ok;
}

/// internal_check_overlap: true when `r` intersects another channel's
/// active region (start_a <= end_b and start_b <= end_a).
pub fn overlaps(states: []const state.ChanState, channel: u8, r: *const state.Region) bool {
    for (states, 0..) |*st, other| {
        if (other == channel or st.active_region_id == state.no_region) continue;
        const live = &st.regions[st.active_region_id];
        if (r.start_addr <= live.end_addr and live.start_addr <= r.end_addr) return true;
    }
    return false;
}

/// ra8_dotf_set_region after the null and channel checks: stage `r` into
/// its slot and mark the slot armed.
pub fn set(states: []state.ChanState, channel: u8, r: *const state.Region) u16 {
    const err = validate(channel, r);
    if (err != ok) return err;
    if (overlaps(states, channel, r)) return conflict;
    const st = &states[channel];
    st.regions[r.region_id] = r.*;
    st.region_valid[r.region_id] = 1;
    return ok;
}

/// ra8_dotf_select_region after the channel check. End is written before
/// start so the end never sits below the start (FSP r_ospi_b.c; HUM
/// 45.3.2 CONVAREAD, 45.3.1 CONVAREAST).
pub fn select(st: *state.ChanState, region_id: u8, regs: anytype) u16 {
    if (region_id >= state.max_regions) return invalid_arg;
    if (st.region_valid[region_id] == 0) return invalid_state;
    const r = &st.regions[region_id];
    regs.writeEnd(r.end_addr & addr_mask);
    regs.writeStart(r.start_addr & addr_mask);
    st.active_region_id = region_id;
    return ok;
}

/// ra8_dotf_get_active_region after the null and channel checks.
pub fn active(st: *const state.ChanState) ?state.Region {
    if (st.active_region_id == state.no_region) return null;
    return st.regions[st.active_region_id];
}
