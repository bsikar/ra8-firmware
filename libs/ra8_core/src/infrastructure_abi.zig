//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for `libs/ra8_core/inc/ra8_infrastructure.h`.
//!
//! Application-layer bring-up, run from `main()` after `SystemInit()` has
//! already dealt with VTOR, the FPU, the caches and priority grouping.
//! Anything needing a peripheral clock belongs in that driver's `_init()`,
//! not here.
//!
//! The three calls reach their targets through `extern`, not through a Zig
//! import, even though all three now live in this same archive. Two of them
//! (`ra8_log_init`, `ra8_log_emit_info`) are WEAK exports that an image is
//! expected to replace; importing the module behind them would bind this
//! file to the default at compile time and quietly route bring-up around
//! whatever the image installed.

const canary = @import("infrastructure_canary");

/// `ra8_err_t` values this module returns, from `inc/ra8_err.h`.
const err = struct {
    pub const ok: u16 = 0;
    pub const validation_failed: u16 = 0x501;
};

const tag: [*:0]const u8 = "INFRA";

extern fn ra8_log_init() void;
extern fn ra8_log_emit_info(tag: [*:0]const u8, message: [*:0]const u8) void;
extern fn ra8_pin_validator_reset() void;

/// Order matters: the log backend comes up first so everything after it can
/// report, and the canary is seeded before any driver init can run deep
/// enough to threaten the stack.
pub export fn ra8_infrastructure_init() callconv(.c) void {
    ra8_log_init();
    ra8_pin_validator_reset();
    canary.seed();
    ra8_log_emit_info(tag, "infrastructure ready");
}

pub export fn ra8_stack_canary_check() callconv(.c) u16 {
    return if (canary.intact()) err.ok else err.validation_failed;
}
