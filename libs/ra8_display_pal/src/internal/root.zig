//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Pure decision logic for the backend-agnostic half of the display PAL: the
//! dispatcher's handle judgement (`ra8_display_pal.c`) and the page-turn
//! refresh cadence (`ra8_display_pal_policy.c`). Nothing here touches MMIO,
//! the heap or a backend; every value type mirrors the C ABI exactly so the
//! membrane in `../ra8_display_pal_abi.zig` only has to dispatch.

const std = @import("std");

/// `ra8_err_t` values this half of the library can answer with.
pub const err_ok: u16 = 0;
pub const err_invalid_arg: u16 = 0x103;
pub const err_busy: u16 = 0x109;
pub const err_null_ptr: u16 = 0x504;

/// `display_pixfmt_t`.
pub const pixfmt_rgb565: u8 = 0;
pub const pixfmt_rgb888: u8 = 1;
pub const pixfmt_grey4: u8 = 2;
pub const pixfmt_grey1: u8 = 3;

/// `display_refresh_hint_t`.
pub const refresh_fast: u8 = 0;
pub const refresh_quality: u8 = 1;
pub const refresh_init: u8 = 2;

/// `display_policy_kind_t`. `fast_clean` is also the highest defined value, so
/// it doubles as the range gate `display_policy_init` applies.
pub const policy_fast_only: u8 = 0;
pub const policy_quality: u8 = 1;
pub const policy_fast_clean: u8 = 2;

/// `display_turn_event_t`. `chapter` is the highest defined value and doubles
/// as the range gate `display_policy_decide` applies.
pub const event_open: u8 = 0;
pub const event_turn: u8 = 1;
pub const event_chapter: u8 = 2;

/// `display_policy_const_t` clamp bounds. The ceiling is 256, so it does not
/// fit a `u8` and the field stays 16-bit exactly as in the C.
pub const clean_every_default: u16 = 8;
pub const clean_every_min: u16 = 1;
pub const clean_every_max: u16 = 256;

/// `display_rect_t`: half-open rectangle in framebuffer coordinates.
pub const Rect = extern struct {
    x: u16 = 0,
    y: u16 = 0,
    w: u16 = 0,
    h: u16 = 0,

    /// The value both `display_full_rect` failure paths answer with.
    pub const empty: Rect = .{ .x = 0, .y = 0, .w = 0, .h = 0 };
};

/// `display_caps_t`: what a backend reports after init.
pub const Caps = extern struct {
    width_px: u16 = 0,
    height_px: u16 = 0,
    pixfmt: u8 = 0,
    stride_bytes: u32 = 0,
    refresh_latency_us_typ: u32 = 0,
    supports_partial_update: bool = false,
    continuous_refresh: bool = false,
};

/// `display_fb_t`: the framebuffer descriptor handed to paint loops.
pub const Fb = extern struct {
    pixels: ?*anyopaque = null,
    width_px: u16 = 0,
    height_px: u16 = 0,
    stride_bytes: u32 = 0,
    pixfmt: u8 = 0,
};

/// `display_policy_t`: caller-owned cadence state. `kind` stays a raw byte,
/// because a caller's zeroed struct is the only guaranteed initial value and
/// nothing may assume the byte names a valid enumerator.
pub const Policy = extern struct {
    kind: u8 = 0,
    clean_every: u16 = 0,
    turns_since_clean: u16 = 0,
};

/// `display_policy_decision_t`: what the app passes to `display_flush`.
pub const Decision = extern struct {
    hint: u8 = 0,
    full_update: bool = false,
};

comptime {
    std.debug.assert(@sizeOf(Rect) == 8);
    std.debug.assert(@offsetOf(Rect, "w") == 4);
    std.debug.assert(@sizeOf(Caps) == 20);
    std.debug.assert(@offsetOf(Caps, "pixfmt") == 4);
    std.debug.assert(@offsetOf(Caps, "stride_bytes") == 8);
    std.debug.assert(@offsetOf(Caps, "refresh_latency_us_typ") == 12);
    std.debug.assert(@offsetOf(Caps, "supports_partial_update") == 16);
    std.debug.assert(@offsetOf(Caps, "continuous_refresh") == 17);
    std.debug.assert(@offsetOf(Fb, "width_px") == @sizeOf(usize));
    std.debug.assert(@offsetOf(Fb, "stride_bytes") == @sizeOf(usize) + 4);
    std.debug.assert(@offsetOf(Fb, "pixfmt") == @sizeOf(usize) + 8);
    std.debug.assert(@sizeOf(Policy) == 6);
    std.debug.assert(@offsetOf(Policy, "clean_every") == 2);
    std.debug.assert(@offsetOf(Policy, "turns_since_clean") == 4);
    std.debug.assert(@sizeOf(Decision) == 2);
    std.debug.assert(@offsetOf(Decision, "full_update") == 1);
}

/// Why the dispatcher refused, or that it accepted. The C's
/// `internal_validate_handle` emits no log line on any arm, so this carries no
/// message with it.
pub const HandleStatus = enum {
    ok,
    /// The caller passed a NULL handle.
    null_handle,
    /// No handle is live, or the pointer is not the one live handle.
    not_the_live_handle,

    pub fn code(self: HandleStatus) u16 {
        return switch (self) {
            .ok => err_ok,
            .null_handle => err_null_ptr,
            .not_the_live_handle => err_invalid_arg,
        };
    }
};

/// `internal_validate_handle`, kept in its three-check order: NULL first, then
/// the init flag, then identity against the one module-static handle. The two
/// refusals share `k_ra8_err_invalid_arg`, so both arms stay reachable here
/// even though the C collapses them at the call site.
pub fn handleStatus(handle_is_null: bool, initialized: bool, is_live_handle: bool) HandleStatus {
    if (handle_is_null) return .null_handle;
    if (!initialized) return .not_the_live_handle;
    if (!is_live_handle) return .not_the_live_handle;
    return .ok;
}

/// `internal_clamp_clean_every`: symmetric clamp onto the documented bounds.
pub fn clampCleanEvery(n: u16) u16 {
    if (n < clean_every_min) return clean_every_min;
    if (n > clean_every_max) return clean_every_max;
    return n;
}

/// The range gate `display_policy_init` applies to its `kind` argument. The C
/// compares against the highest enumerator rather than listing the valid ones.
pub fn policyKindInRange(kind: u8) bool {
    return kind <= policy_fast_clean;
}

/// The range gate `display_policy_decide` applies to its `event` argument.
pub fn turnEventInRange(event: u8) bool {
    return event <= event_chapter;
}

/// `internal_decide_fast_clean`'s MC/DC decision:
/// `(turns_since_clean + 1 >= clean_every) || (event == chapter)`.
/// The `+ 1` is `uint16_t` arithmetic in the C, so it wraps rather than traps.
pub fn fastCleanTurn(turns_since_clean: u16, clean_every: u16, event: u8) bool {
    const next: u16 = turns_since_clean +% 1;
    return (next >= clean_every) or (event == event_chapter);
}

/// `internal_decide_fast_clean` whole: the decision written out and the
/// counter moved. A clean turn resets the counter and asks for a quality full
/// update; a fast turn advances it and asks for a fast partial update.
pub fn decideFastClean(policy: *Policy, event: u8) Decision {
    const next: u16 = policy.turns_since_clean +% 1;
    if (fastCleanTurn(policy.turns_since_clean, policy.clean_every, event)) {
        policy.turns_since_clean = 0;
        return .{ .hint = refresh_quality, .full_update = true };
    }
    policy.turns_since_clean = next;
    return .{ .hint = refresh_fast, .full_update = false };
}

/// `display_policy_decide` past its guards: the open-event shortcut first,
/// then the per-kind switch. The `default` arm of the C switch falls into the
/// fast/clean cadence, so any byte that is not `fast_only` or `quality` lands
/// there, which is what a caller's zeroed-then-corrupted state hits.
pub fn decide(policy: *Policy, event: u8) Decision {
    if (event == event_open) {
        policy.turns_since_clean = 0;
        return .{ .hint = refresh_init, .full_update = true };
    }
    return switch (policy.kind) {
        policy_fast_only => .{ .hint = refresh_fast, .full_update = false },
        policy_quality => .{ .hint = refresh_quality, .full_update = true },
        else => decideFastClean(policy, event),
    };
}

/// The gate `display_policy_full_rect` applies once `out` is known good.
pub fn fullRectDimensionsValid(w: u16, h: u16) bool {
    return (w != 0) and (h != 0);
}

/// The rectangle `display_full_rect` builds from a backend's capabilities.
pub fn rectFromCaps(caps: Caps) Rect {
    return .{ .x = 0, .y = 0, .w = caps.width_px, .h = caps.height_px };
}
