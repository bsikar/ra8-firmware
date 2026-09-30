//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Archive root for `ra8_camera`. The facade and each ported backend live in
//! separate files, so each needs an explicit comptime reference or its exports
//! never reach the static library.
//!
//! The CEU capture source is in here too now, so the library has no C
//! translation unit left. The two host suites that used to white-box a copy of
//! the C file reach its poll loop and capture entry through the `priv_cam_ceu_`
//! symbols declared in `src/ra8_camera_source_ceu_private.h`.

comptime {
    _ = @import("ra8_camera_abi.zig");
    _ = @import("source_memory.zig");
    _ = @import("source_ceu.zig");
    _ = @import("codec_passthrough.zig");
    _ = @import("codec_jpeg_sw.zig");
}
