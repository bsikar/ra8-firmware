//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for ra8_dotf_install_key / set_iv / rotate_key (RA8FW-840). The
//! logic is in internal/dotf_key.zig; the state table is defined in
//! dotf_state_abi.zig.

const common = @import("abi_common.zig");
const key = @import("internal/dotf_key.zig");
const power = @import("internal/dotf_power.zig");
const state = @import("internal/dotf_state.zig");
const Regs = @import("dotf_regs.zig").Regs;

const tag = "DOTF";

extern var s_dotf_state: [power.channel_count]state.ChanState;

fn nullPtr(msg: [*:0]const u8) u16 {
    common.ra8_log_emit_error(tag, msg);
    return common.k_ra8_err_null_ptr;
}

export fn ra8_dotf_install_key(channel: u8, handle: ?*const state.KeyHandle) u16 {
    const h = handle orelse return nullPtr("handle must not be nullptr");
    if (!power.channelInRange(channel)) return common.k_ra8_err_invalid_arg;
    const err = key.installKey(&s_dotf_state[channel], Regs.of(channel), h);
    if (err == common.k_ra8_ok) common.ra8_log_emit_info_val(tag, "install_key channel", channel);
    return err;
}

export fn ra8_dotf_set_iv(channel: u8, iv_words: ?*const key.Iv) u16 {
    const iv = iv_words orelse return nullPtr("iv_words must not be nullptr");
    if (!power.channelInRange(channel)) return common.k_ra8_err_invalid_arg;
    key.setIv(&s_dotf_state[channel], Regs.of(channel), iv);
    return common.k_ra8_ok;
}

export fn ra8_dotf_rotate_key(channel: u8, new_handle: ?*const state.KeyHandle, iv_words: ?*const key.Iv) u16 {
    const h = new_handle orelse return nullPtr("new_handle must not be nullptr");
    const err = if (power.channelInRange(channel)) key.validateRotate(&s_dotf_state[channel], h) else common.k_ra8_err_invalid_arg;
    if (err != common.k_ra8_ok) {
        // RA8_RETURN_ON_ERROR: message, then the code.
        common.ra8_log_emit_error(tag, "rotate_key: validation failed");
        common.ra8_log_emit_error_val(tag, "Error", err);
        return err;
    }
    key.rotate(&s_dotf_state[channel], Regs.of(channel), h, iv_words);
    common.ra8_log_emit_info_val(tag, "rotate_key channel", channel);
    return common.k_ra8_ok;
}
