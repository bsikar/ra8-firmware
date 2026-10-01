//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for `libs/ra8_core/inc/ra8_boot_region.h`.
//!
//! The header's contract is three things and this file keeps all three: the
//! span is half-open, both pointers are rejected as `nullptr` with a log
//! line before anything is written, and `end` preceding `start` is
//! `invalid_arg` rather than a wild fill. The C reached the first two through
//! `RA8_CHECK_NULL_PTR`, which is the same log-then-return expanded inline.
//!
//! `ra8_boot_test_sdram_window` is exported off target only, exactly as the
//! C guarded it with `#ifdef RA8_OFF_TARGET`. On an image there is no
//! stand-in window to hand anyone.

const builtin = @import("builtin");

const region = @import("boot_region");

/// Whether this build is an image rather than a host test binary.
const on_target = builtin.target.os.tag == .freestanding;

/// `ra8_err_t` values this module returns, from `inc/ra8_err.h`.
const err = struct {
    pub const ok: u16 = 0;
    pub const invalid_arg: u16 = 0x103;
    pub const null_ptr: u16 = 0x504;
    pub const validation_failed: u32 = 0x501;
};

const tag: [*:0]const u8 = "boot_region";

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;
extern fn ra8_fatal_error(tag: [*:0]const u8, message: [*:0]const u8, err: u32) noreturn;

pub export fn ra8_boot_zero_region(start: ?*anyopaque, end: ?*const anyopaque) callconv(.c) u16 {
    const first = start orelse {
        ra8_log_emit_error(tag, "start must not be nullptr");
        return err.null_ptr;
    };
    const last = end orelse {
        ra8_log_emit_error(tag, "end must not be nullptr");
        return err.null_ptr;
    };

    const bytes = region.spanLength(@intFromPtr(first), @intFromPtr(last)) orelse {
        ra8_log_emit_error(tag, "zero_region: end precedes start");
        return err.invalid_arg;
    };

    const base: [*]u8 = @ptrCast(first);
    region.zero(base[0..bytes]);
    return err.ok;
}

pub export fn ra8_boot_zero_sdram_bss() callconv(.c) u16 {
    const section = region.sdramSection();
    region.zero(section);
    return err.ok;
}

comptime {
    if (!on_target) {
        @export(&testSdramWindow, .{ .name = "ra8_boot_test_sdram_window", .linkage = .strong });
    }
}

/// Host-test hook: hand the suite the stand-in window so it can dirty it and
/// then assert the fill. `RA8_ASSERT` on the out-parameter in the C, which is
/// the fatal path, so this is too.
fn testSdramWindow(out_bytes: ?*usize) callconv(.c) [*]u8 {
    const out = out_bytes orelse
        ra8_fatal_error("ASSERT", "out_bytes must not be nullptr", err.validation_failed);
    const window = region.sdramSection();
    out.* = window.len;
    return window.ptr;
}
