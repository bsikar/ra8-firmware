//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The ra8_flash runtime state owned by ra8_flash.c (ra8_flash_internal.h),
//! shared by the flash *_abi.zig units. Declarations only.

const common = @import("abi_common.zig");

const Callback = *const fn (?*const anyopaque) callconv(.c) void;

/// ra8_flash_runtime_t.
pub const Runtime = extern struct {
    cb: ?Callback,
    user_ctx: ?*anyopaque,
    initialized: bool,
    prefetch_on: bool,
    win_low: usize,
    win_high: usize,
};

comptime {
    if (@offsetOf(Runtime, "initialized") != 2 * @sizeOf(usize)) @compileError("ra8_flash_runtime_t layout");
}

pub extern var g_flash_rt: Runtime;
pub extern var g_flash_tag: [*:0]const u8;

/// RA8_VALIDATE_INIT: false (after logging) when ra8_flash_init has not run.
pub fn ready(msg: [*:0]const u8) bool {
    if (g_flash_rt.initialized) return true;
    common.ra8_log_emit_error(g_flash_tag, msg);
    return false;
}

/// RA8_CHECK_NULL_PTR: false (after logging) for a null pointer.
pub fn present(ptr: ?*const anyopaque, msg: [*:0]const u8) bool {
    if (ptr != null) return true;
    common.ra8_log_emit_error(g_flash_tag, msg);
    return false;
}
