//! An M85 (CPU0) ThreadX Module Manager app (RA8FW-796, under RA8FW-794).
//!
//! An app sets `CrossApp.txm_module` to the name of a module in
//! `cpu1_txm_hello.modules`. Two things then change for its M85 image and
//! nothing else does:
//!
//! - the kernel it links is `threadx_m85_modules` (RA8FW-426) in place of
//!   the plain `threadx`, so the Module Manager, the RA8FW-481 scheduler and
//!   the RA8FW-484 MPU budget come with it, and the app's own units compile
//!   with the Module Manager's defines and include directories;
//! - the module blob is built and packed into `.txm_module`, the same object
//!   a CPU1 manager image links (RA8FW-431), and joins the M85 link.
//!
//! An app without the field resolves exactly as before, so every existing
//! image is unchanged.

const std = @import("std");
const middleware = @import("middleware.zig");
const m85_threadx_modules = @import("m85_threadx_modules.zig");

/// The middleware set an M85 app links: `mws` unchanged, unless the app is a
/// Module Manager, in which case `threadx` becomes `threadx_m85_modules` in
/// the same position. A manager app that does not use ThreadX at all is a
/// table mistake, so it panics rather than linking a kernel the app never
/// asked for.
pub fn kernelFor(allocator: std.mem.Allocator, mws: []const middleware.Middleware, manager: bool) []const middleware.Middleware {
    if (!manager) return mws;
    const out = allocator.dupe(middleware.Middleware, mws) catch @panic("OOM");
    var swapped = false;
    for (out) |*mw| {
        if (std.mem.eql(u8, mw.name, middleware.threadx.name)) {
            mw.* = m85_threadx_modules.threadx_m85_modules;
            swapped = true;
        }
    }
    if (!swapped) @panic("an M85 Module Manager app (txm_module set) must name threadx in USES");
    return out;
}
