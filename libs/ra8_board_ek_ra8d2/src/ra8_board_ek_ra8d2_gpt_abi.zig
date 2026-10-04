//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The C ABI of the board GPT profile: the six names
//! `inc/ra8_board_ek_ra8d2_gpt_profile.h` declares, unchanged. Apart from
//! `ra8_board_ek_ra8d2_abi.zig` only to keep both files short.

const gpt_profile = @import("internal/gpt_profile.zig");

export fn ra8_board_timer_to_chip(index: u8, out_chip: ?*u8) gpt_profile.ErrCode {
    return gpt_profile.timerToChip(index, out_chip);
}

export fn ra8_board_pwm_to_chip(index: u8, out_chip: ?*u8) gpt_profile.ErrCode {
    return gpt_profile.pwmToChip(index, out_chip);
}

export fn ra8_board_timer_profile_bind(tmr: ?*gpt_profile.FwTimer) gpt_profile.ErrCode {
    return gpt_profile.bindTimer(tmr);
}

export fn ra8_board_pwm_profile_bind(pwm: ?*gpt_profile.FwPwm) gpt_profile.ErrCode {
    return gpt_profile.bindPwm(pwm);
}

export fn ra8_board_timer() *const gpt_profile.FwTimer {
    return gpt_profile.timerHandle();
}

export fn ra8_board_pwm() *const gpt_profile.FwPwm {
    return gpt_profile.pwmHandle();
}
