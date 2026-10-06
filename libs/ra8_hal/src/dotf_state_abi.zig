//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the DOTF state table and ra8_dotf_clear_status (RA8FW-835).
//! s_dotf_state is defined here under its C name; the other dotf_*_abi.zig
//! units read and write it through an extern declaration.

const common = @import("abi_common.zig");
const power = @import("internal/dotf_power.zig");
const state = @import("internal/dotf_state.zig");
const status = @import("internal/dotf_status.zig");

/// Per-channel state, zeroed like the C static it replaces.
export var s_dotf_state: [power.channel_count]state.ChanState = [_]state.ChanState{.{}} ** power.channel_count;

const Reg = struct {
    p: *volatile u32,
    pub fn write(self: Reg, v: u32) void {
        self.p.* = v;
    }
};

export fn ra8_dotf_clear_status(channel: u8) u16 {
    if (!power.channelInRange(channel)) return common.k_ra8_err_invalid_arg;
    const r = Reg{ .p = @ptrFromInt(status.reg00(channel)) };
    state.clearStatus(r, &s_dotf_state[channel]);
    return common.k_ra8_ok;
}
