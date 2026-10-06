//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/dotf_state.zig.

const std = @import("std");
const state = @import("dotf_state");

const FakeReg = struct {
    value: u32 = 0xFFFF_FFFF,
    writes: u32 = 0,
    pub fn write(self: *FakeReg, v: u32) void {
        self.value = v;
        self.writes += 1;
    }
};

test "layout matches ra8_dotf_chan_state_t" {
    try std.testing.expectEqual(@as(usize, 124), @sizeOf(state.ChanState));
    try std.testing.expectEqual(@as(usize, 52), @offsetOf(state.ChanState, "active_region_id"));
    try std.testing.expectEqual(@as(usize, 96), @offsetOf(state.ChanState, "iv_cache"));
    try std.testing.expectEqual(@as(usize, 120), @offsetOf(state.ChanState, "cached_sca"));
    try std.testing.expectEqual(@as(usize, 8), @offsetOf(state.KeyHandle, "words"));
    try std.testing.expectEqual(@as(usize, 8), @offsetOf(state.Region, "key_index"));
}

test "default state is all zero, like the C static" {
    const st = state.ChanState{};
    const bytes = std.mem.asBytes(&st);
    for (bytes, 0..) |b, i| {
        if (i >= 53 and i < 56) continue; // padding
        if (i >= 113 and i < 116) continue;
        if (i >= 122) continue;
        try std.testing.expectEqual(@as(u8, 0), b);
    }
}

test "clearStatus zeroes REG00 once and disarms only enabled" {
    var reg = FakeReg{};
    var st = state.ChanState{};
    st.enabled = 1;
    st.iv_valid = 1;
    st.active_region_id = 2;
    state.clearStatus(&reg, &st);
    try std.testing.expectEqual(@as(u32, 0), reg.value);
    try std.testing.expectEqual(@as(u32, 1), reg.writes);
    try std.testing.expectEqual(@as(u8, 0), st.enabled);
    try std.testing.expectEqual(@as(u8, 1), st.iv_valid);
    try std.testing.expectEqual(@as(u8, 2), st.active_region_id);
}

test "reset matches internal_state_reset" {
    var st = state.ChanState{};
    st.enabled = 1;
    st.iv_valid = 1;
    st.region_valid[3] = 1;
    st.regions[3].end_addr = 0x1FFF;
    st.key.words[7] = 0xDEAD;
    st.key.valid = 1;
    state.reset(&st);
    try std.testing.expectEqual(state.no_region, st.active_region_id);
    try std.testing.expectEqual(state.key_size_128, st.key.size);
    try std.testing.expectEqual(state.key_size_128, st.cached_key_size);
    try std.testing.expectEqual(state.sca_standard, st.cached_sca);
    try std.testing.expectEqual(@as(u8, 0), st.enabled);
    try std.testing.expectEqual(@as(u8, 0), st.iv_valid);
    try std.testing.expectEqual(@as(u8, 0), st.region_valid[3]);
    try std.testing.expectEqual(@as(u32, 0), st.regions[3].end_addr);
    try std.testing.expectEqual(@as(u32, 0), st.key.words[7]);
    try std.testing.expectEqual(@as(u8, 0), st.key.valid);
}
