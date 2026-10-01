//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Archive root for `ra8_rot_launch`: the authenticating copy-to-run
//! hand-off, `ra8_dfu_launch`.
//!
//! Its own archive because linking it is a distinct opt-in from linking the
//! verifier. An app that only verifies (`rot_verify_hil`) or runs in the
//! non-secure world (`secure_boot_ns_hil`) links `ra8_rot` and never this,
//! so it does not inherit the `ra8_dfu` archive the hand-off needs.

comptime {
    _ = @import("launch_gate_abi");
}
