//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Root of the `ra8_widget_host` package export (RA8FW-828): the CPU canvas
//! for host previews, plus the box layout, UI rect and widget core symbols the
//! widgets reach through `extern fn`, so an importer links them from Zig
//! instead of the board build. The importer still exports
//! `ra8_log_emit_error`, which belongs to its own log.

pub const Canvas = @import("host_paint.zig").Canvas;

comptime {
    _ = @import("box");
    _ = @import("ui");
    _ = @import("ra8_widget").core;
}
