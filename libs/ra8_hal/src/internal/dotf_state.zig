//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! DOTF per-channel software state (RA8FW-835, was part of ra8_dotf.c). The
//! layout matches the old ra8_dotf_chan_state_t (the C tests still see it
//! only through the API); the table is exported from src/dotf_state_abi.zig.

pub const max_regions = 4;
pub const iv_words = 4;
pub const no_region: u8 = 0xFF;
pub const reg00_disable: u32 = 0;
/// k_ra8_dotf_key_size_128 (REG00 key-size field, FSP default).
pub const key_size_128: u32 = 0x0200_0000;
/// k_ra8_dotf_sca_standard.
pub const sca_standard: u8 = 1;

/// ra8_dotf_region_t.
pub const Region = extern struct {
    start_addr: u32 = 0,
    end_addr: u32 = 0,
    key_index: u8 = 0,
    region_id: u8 = 0,
};

/// ra8_dotf_key_handle_t; `size` is the u32-backed ra8_dotf_key_size_t.
pub const KeyHandle = extern struct {
    size: u32 = 0,
    key_index: u8 = 0,
    valid: u8 = 0,
    words: [8]u32 = [_]u32{0} ** 8,
};

/// ra8_dotf_chan_state_t.
pub const ChanState = extern struct {
    regions: [max_regions]Region = [_]Region{.{}} ** max_regions,
    region_valid: [max_regions]u8 = [_]u8{0} ** max_regions,
    active_region_id: u8 = 0,
    key: KeyHandle = .{},
    iv_cache: [iv_words]u32 = [_]u32{0} ** iv_words,
    iv_valid: u8 = 0,
    cached_key_size: u32 = 0,
    cached_sca: u8 = 0,
    enabled: u8 = 0,
};

comptime {
    // Measured from the C struct with offsetof (same on host and ARM).
    if (@sizeOf(Region) != 12 or @sizeOf(KeyHandle) != 40) @compileError("dotf leaf layout");
    if (@sizeOf(ChanState) != 124) @compileError("dotf state size");
    if (@offsetOf(ChanState, "region_valid") != 48 or @offsetOf(ChanState, "key") != 56 or
        @offsetOf(ChanState, "iv_valid") != 112 or @offsetOf(ChanState, "cached_key_size") != 116 or
        @offsetOf(ChanState, "enabled") != 121) @compileError("dotf state offsets");
}

/// Clearing REG00 wipes the AES enable and status bits, so the channel is
/// no longer armed. HUM Ch 45.3 "Register Descriptions" p 3049.
pub fn clearStatus(reg: anytype, st: *ChanState) void {
    reg.write(reg00_disable);
    st.enabled = 0;
}

/// internal_state_reset: every slot disarmed, no active region, a cleared
/// 128-bit key, no IV, standard side-channel level, channel disarmed.
pub fn reset(st: *ChanState) void {
    st.* = .{
        .active_region_id = no_region,
        .key = .{ .size = key_size_128 },
        .cached_key_size = key_size_128,
        .cached_sca = sca_standard,
    };
}
