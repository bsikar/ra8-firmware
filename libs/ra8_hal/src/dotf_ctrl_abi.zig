//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for ra8_dotf_enable / disable / set_sca_level / set_key_size
//! (RA8FW-840). The logic is in internal/dotf_ctrl.zig; the state table is
//! defined in dotf_state_abi.zig.

const common = @import("abi_common.zig");
const ctrl = @import("internal/dotf_ctrl.zig");
const power = @import("internal/dotf_power.zig");
const state = @import("internal/dotf_state.zig");
const Regs = @import("dotf_regs.zig").Regs;

const tag = "DOTF";

extern var s_dotf_state: [power.channel_count]state.ChanState;

export fn ra8_dotf_enable(channel: u8) u16 {
    if (!power.channelInRange(channel)) return common.k_ra8_err_invalid_arg;
    ctrl.enable(&s_dotf_state[channel], Regs.of(channel));
    common.ra8_log_emit_info_val(tag, "enable channel", channel);
    return common.k_ra8_ok;
}

export fn ra8_dotf_disable(channel: u8) u16 {
    if (!power.channelInRange(channel)) return common.k_ra8_err_invalid_arg;
    ctrl.disable(&s_dotf_state[channel], Regs.of(channel));
    return common.k_ra8_ok;
}

export fn ra8_dotf_set_sca_level(channel: u8, level: u8) u16 {
    if (!power.channelInRange(channel)) return common.k_ra8_err_invalid_arg;
    return ctrl.setSca(&s_dotf_state[channel], Regs.of(channel), level);
}

export fn ra8_dotf_set_key_size(channel: u8, size: u32) u16 {
    if (!power.channelInRange(channel)) return common.k_ra8_err_invalid_arg;
    return ctrl.setKeySize(&s_dotf_state[channel], Regs.of(channel), size);
}
