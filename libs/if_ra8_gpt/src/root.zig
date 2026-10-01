//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Archive root for `if_ra8_gpt`, the RA8 GPT32 adapters behind the neutral
//! `fw_if_timer` and `fw_if_pwm` ports. Referencing both membranes is what
//! keeps their exports in the archive.

comptime {
    _ = @import("timer_ra8_abi");
    _ = @import("pwm_ra8_abi");
}
