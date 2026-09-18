//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for the backend-agnostic half of the display PAL:
//! `inc/ra8_display_pal.h` (the dispatcher) and
//! `inc/ra8_display_pal_policy.h` (the page-turn refresh cadence). The
//! decision logic lives in `internal/root.zig`; this file owns the exported
//! symbols, the module-static handle, the argument guards in their original
//! order and the `ra8_err_t` mapping.
//!
//! The panel is a caller-supplied seam: every operation dispatches through the
//! `display_backend_iface` rows a backend exported by address, so this
//! translation unit names no controller and links against no driver, exactly
//! as the C did. The LCD/GLCDC and IT8951 e-ink backends stay C on this
//! branch and bind here at run time through `display_cfg_t.iface`.

const std = @import("std");
const implementation = @import("internal/root.zig");

/// `display_rect_t`.
pub const Rect = implementation.Rect;
/// `display_caps_t`.
pub const Caps = implementation.Caps;
/// `display_fb_t`.
pub const Fb = implementation.Fb;
/// `display_policy_t`.
pub const Policy = implementation.Policy;
/// `display_policy_decision_t`.
pub const Decision = implementation.Decision;

/// Log tag on the dispatcher's lines, matching the C's `s_tag`.
const tag: [*:0]const u8 = "ra8_display_pal";
/// Log tag on the policy's lines. It is deliberately a DIFFERENT tag from the
/// dispatcher's, exactly as the two C translation units had.
const tag_policy: [*:0]const u8 = "disp_policy";

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;
extern fn ra8_log_emit_info(tag: [*:0]const u8, message: [*:0]const u8) void;

/// Log a rejected pointer the way `RA8_CHECK_NULL_PTR` did, then answer
/// `k_ra8_err_null_ptr`.
fn nullPtr(which_tag: [*:0]const u8, message: [*:0]const u8) u16 {
    ra8_log_emit_error(which_tag, message);
    return implementation.err_null_ptr;
}

/// `struct display_backend_iface`, the vtable every backend fills in. Every
/// row is optional here because the C struct holds plain function pointers a
/// caller may leave null, and `display_init` rejects a missing `init`.
pub const BackendIface = extern struct {
    init: ?*const fn (cfg: ?*const Config, out_ctx: ?*?*anyopaque) callconv(.c) u16 = null,
    get_caps: ?*const fn (ctx: ?*const anyopaque, out: ?*Caps) callconv(.c) u16 = null,
    get_framebuffer: ?*const fn (ctx: ?*anyopaque, out: ?*Fb) callconv(.c) u16 = null,
    flush: ?*const fn (ctx: ?*anyopaque, rect: Rect, hint: u8) callconv(.c) u16 = null,
    clear: ?*const fn (ctx: ?*anyopaque, color: u32) callconv(.c) u16 = null,
    deinit: ?*const fn (ctx: ?*anyopaque) callconv(.c) u16 = null,
};

/// `display_cfg_t`, the descriptor a caller hands `display_init`.
pub const Config = extern struct {
    iface: ?*const BackendIface = null,
    framebuffer: ?*anyopaque = null,
    framebuffer_bytes: u32 = 0,
    width_px: u16 = 0,
    height_px: u16 = 0,
    pixfmt: u8 = 0,
    panel_timing: ?*const anyopaque = null,
};

/// `struct display_handle`, the concrete definition behind the opaque handle
/// applications see. One module-static instance, one display per board.
pub const Handle = extern struct {
    iface: ?*const BackendIface = null,
    ctx: ?*anyopaque = null,
};

comptime {
    std.debug.assert(@sizeOf(Handle) == @sizeOf(usize) * 2);
    std.debug.assert(@offsetOf(Handle, "ctx") == @sizeOf(usize));
    std.debug.assert(@offsetOf(Config, "framebuffer") == @sizeOf(usize));
    std.debug.assert(@offsetOf(Config, "framebuffer_bytes") == @sizeOf(usize) * 2);
    std.debug.assert(@sizeOf(BackendIface) == @sizeOf(usize) * 6);
}

/// The single PAL handle, and the flag that says whether it is live.
var s_handle: Handle = .{};
var s_initialized: bool = false;

/// `internal_validate_handle`: NULL, then the init flag, then identity against
/// the one live handle. No log line on any arm, exactly as the C.
fn validateHandle(d: ?*const Handle) u16 {
    const status = implementation.handleStatus(
        d == null,
        s_initialized,
        d == @as(?*const Handle, &s_handle),
    );
    return status.code();
}

pub export fn display_init(cfg: ?*const Config, out_handle: ?*?*Handle) callconv(.c) u16 {
    const config = cfg orelse return nullPtr(tag, "cfg must not be nullptr");
    const out = out_handle orelse return nullPtr(tag, "out_handle must not be nullptr");
    const iface = config.iface orelse return nullPtr(tag, "cfg->iface must not be nullptr");
    const init_fn = iface.init orelse return nullPtr(tag, "iface->init must not be nullptr");

    if (s_initialized) {
        ra8_log_emit_error(tag, "display_init: PAL already initialised");
        return implementation.err_busy;
    }

    var backend_ctx: ?*anyopaque = null;
    const err = init_fn(config, &backend_ctx);
    if (err != implementation.err_ok) {
        return err;
    }

    s_handle.iface = iface;
    s_handle.ctx = backend_ctx;
    s_initialized = true;
    out.* = &s_handle;
    ra8_log_emit_info(tag, "display_init: backend bound");
    return implementation.err_ok;
}

pub export fn display_get_caps(d: ?*const Handle, out: ?*Caps) callconv(.c) u16 {
    const v = validateHandle(d);
    if (v != implementation.err_ok) return v;
    const destination = out orelse return nullPtr(tag, "out must not be nullptr");
    const iface = s_handle.iface.?;
    return iface.get_caps.?(s_handle.ctx, destination);
}

pub export fn display_get_framebuffer(d: ?*const Handle, out: ?*Fb) callconv(.c) u16 {
    const v = validateHandle(d);
    if (v != implementation.err_ok) return v;
    const destination = out orelse return nullPtr(tag, "out must not be nullptr");
    const iface = s_handle.iface.?;
    return iface.get_framebuffer.?(s_handle.ctx, destination);
}

pub export fn display_flush(d: ?*const Handle, rect: Rect, hint: u8) callconv(.c) u16 {
    const v = validateHandle(d);
    if (v != implementation.err_ok) return v;
    const iface = s_handle.iface.?;
    return iface.flush.?(s_handle.ctx, rect, hint);
}

pub export fn display_clear(d: ?*const Handle, color: u32) callconv(.c) u16 {
    const v = validateHandle(d);
    if (v != implementation.err_ok) return v;
    const iface = s_handle.iface.?;
    return iface.clear.?(s_handle.ctx, color);
}

pub export fn display_deinit(d: ?*const Handle) callconv(.c) u16 {
    const v = validateHandle(d);
    if (v != implementation.err_ok) return v;
    const iface = s_handle.iface.?;
    const err = iface.deinit.?(s_handle.ctx);
    // Always drop the handle on deinit so a follow-up init can run, even if
    // the backend reported a tear-down error. The deinit error is still
    // returned to the caller.
    s_handle.iface = null;
    s_handle.ctx = null;
    s_initialized = false;
    return err;
}

pub export fn display_full_rect(d: ?*const Handle) callconv(.c) Rect {
    if (validateHandle(d) != implementation.err_ok) return Rect.empty;
    var caps: Caps = .{};
    const iface = s_handle.iface.?;
    if (iface.get_caps.?(s_handle.ctx, &caps) != implementation.err_ok) return Rect.empty;
    return implementation.rectFromCaps(caps);
}

pub export fn display_policy_init(p: ?*Policy, kind: u8, clean_every: u16) callconv(.c) u16 {
    const policy = p orelse return nullPtr(tag_policy, "init: null policy");
    if (!implementation.policyKindInRange(kind)) {
        ra8_log_emit_error(tag_policy, "init: kind out of range");
        return implementation.err_invalid_arg;
    }
    policy.kind = kind;
    policy.clean_every = implementation.clampCleanEvery(clean_every);
    policy.turns_since_clean = 0;
    return implementation.err_ok;
}

pub export fn display_policy_decide(p: ?*Policy, event: u8, out: ?*Decision) callconv(.c) u16 {
    const policy = p orelse return nullPtr(tag_policy, "decide: null policy");
    const destination = out orelse return nullPtr(tag_policy, "decide: null out");
    if (!implementation.turnEventInRange(event)) {
        ra8_log_emit_error(tag_policy, "decide: event out of range");
        return implementation.err_invalid_arg;
    }
    destination.* = implementation.decide(policy, event);
    return implementation.err_ok;
}

pub export fn display_policy_full_rect(w: u16, h: u16, out: ?*Rect) callconv(.c) u16 {
    const destination = out orelse return nullPtr(tag_policy, "full_rect: null out");
    if (!implementation.fullRectDimensionsValid(w, h)) {
        ra8_log_emit_error(tag_policy, "full_rect: zero dimension");
        return implementation.err_invalid_arg;
    }
    destination.* = .{ .x = 0, .y = 0, .w = w, .h = h };
    return implementation.err_ok;
}

/// Test-only reset of the module-static state, so the ABI suite can drive the
/// init/deinit lifecycle more than once in one process. Not exported.
pub fn testResetState() void {
    s_handle = .{};
    s_initialized = false;
}

/// Test-only read of the live handle address.
pub fn testLiveHandle() *Handle {
    return &s_handle;
}

/// Test-only read of the init flag.
pub fn testInitialized() bool {
    return s_initialized;
}
