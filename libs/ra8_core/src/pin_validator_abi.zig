//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for `libs/ra8_core/inc/ra8_pin_validator.h`.
//!
//! The registry behind this file deals in slots and Zig errors. This one
//! maps them onto the `ra8_err_t` codes the header promises, and emits the
//! two log lines the C emitted, through `ra8_log_emit_error` in ra8_log.c,
//! which is still C.

const registry = @import("pin_validator_registry");

/// `ra8_err_t` values this module returns, from `inc/ra8_err.h`.
const err = struct {
    pub const ok: c_int = 0;
    pub const gpio_conflict: c_int = 0x205;
    pub const gpio_invalid_port: c_int = 0x206;
    pub const gpio_invalid_pin: c_int = 0x207;
    pub const null_ptr: c_int = 0x504;
};

const tag: [*:0]const u8 = "PINVAL";

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;

var state: registry.Registry = .{};

fn errorFor(e: registry.IndexError) c_int {
    return switch (e) {
        registry.IndexError.InvalidPort => err.gpio_invalid_port,
        registry.IndexError.InvalidPin => err.gpio_invalid_pin,
    };
}

pub export fn ra8_pin_validator_claim(pin: u16, owner: ?*const anyopaque) callconv(.c) c_int {
    const held = owner orelse {
        ra8_log_emit_error(tag, "owner must not be nullptr");
        return err.null_ptr;
    };

    const slot = registry.slotOf(pin) catch |e| return errorFor(e);

    state.claim(slot, held) catch {
        ra8_log_emit_error(tag, "pin already claimed");
        return err.gpio_conflict;
    };
    return err.ok;
}

pub export fn ra8_pin_validator_release(pin: u16) callconv(.c) c_int {
    const slot = registry.slotOf(pin) catch |e| return errorFor(e);
    state.release(slot);
    return err.ok;
}

pub export fn ra8_pin_validator_is_claimed(pin: u16) callconv(.c) bool {
    const slot = registry.slotOf(pin) catch return false;
    return state.isClaimed(slot);
}

pub export fn ra8_pin_validator_reset() callconv(.c) void {
    state.reset();
}
