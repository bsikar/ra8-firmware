//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for DOTF handler attach and dispatch (RA8FW-834). s_dotf_fn and
//! s_dotf_ctx are defined here under their C names; dotf_life_abi.zig clears
//! them in init/deinit through extern declarations.

const common = @import("abi_common.zig");
const handler = @import("internal/dotf_handler.zig");

/// Active event callback; null means none.
export var s_dotf_fn: ?handler.EventFn = null;
/// Context handed to s_dotf_fn.
export var s_dotf_ctx: ?*anyopaque = null;

export fn ra8_dotf_attach_handler(f: ?handler.EventFn, ctx: ?*anyopaque) u16 {
    s_dotf_fn = f;
    s_dotf_ctx = ctx;
    return common.k_ra8_ok;
}

export fn ra8_dotf_dispatch(channel: u8) void {
    handler.dispatch(s_dotf_fn, s_dotf_ctx, channel);
}
