//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for ra8_dotf_set_region / select_region / get_active_region
//! (RA8FW-839). The logic is in internal/dotf_region.zig; the state table is
//! defined in dotf_state_abi.zig.

const common = @import("abi_common.zig");
const power = @import("internal/dotf_power.zig");
const region = @import("internal/dotf_region.zig");
const state = @import("internal/dotf_state.zig");
const status = @import("internal/dotf_status.zig");

const tag = "DOTF";
const off_convareast: usize = 0x0;
const off_convaread: usize = 0x4;

extern var s_dotf_state: [power.channel_count]state.ChanState;
extern fn ra8_log_emit_warn_val(tag: [*:0]const u8, message: [*:0]const u8, value: u32) void;

fn nullPtr(msg: [*:0]const u8) u16 {
    common.ra8_log_emit_error(tag, msg);
    return common.k_ra8_err_null_ptr;
}

const Regs = struct {
    base: usize,
    fn word(self: Regs, off: usize) *volatile u32 {
        return @ptrFromInt(self.base + off);
    }
    pub fn writeEnd(self: Regs, v: u32) void {
        self.word(off_convaread).* = v;
    }
    pub fn writeStart(self: Regs, v: u32) void {
        self.word(off_convareast).* = v;
    }
};

export fn ra8_dotf_set_region(channel: u8, r: ?*const state.Region) u16 {
    const reg = r orelse return nullPtr("region must not be nullptr");
    if (!power.channelInRange(channel)) return common.k_ra8_err_invalid_arg;
    const err = region.set(&s_dotf_state, channel, reg);
    if (err == region.conflict) ra8_log_emit_warn_val(tag, "set_region overlaps other channel", channel);
    if (err == common.k_ra8_ok) common.ra8_log_emit_info_val(tag, "set_region staged channel", channel);
    return err;
}

export fn ra8_dotf_select_region(channel: u8, region_id: u8) u16 {
    if (!power.channelInRange(channel)) return common.k_ra8_err_invalid_arg;
    const regs = Regs{ .base = status.base + @as(usize, channel) * status.stride };
    return region.select(&s_dotf_state[channel], region_id, regs);
}

export fn ra8_dotf_get_active_region(channel: u8, out: ?*state.Region) u16 {
    const dst = out orelse return nullPtr("region must not be nullptr");
    if (!power.channelInRange(channel)) return common.k_ra8_err_invalid_arg;
    dst.* = region.active(&s_dotf_state[channel]) orelse return common.k_ra8_err_invalid_state;
    return common.k_ra8_ok;
}
