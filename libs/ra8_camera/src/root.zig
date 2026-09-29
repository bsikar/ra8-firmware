//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Archive root for `ra8_camera`. The facade and each ported backend live in
//! separate files, so each needs an explicit comptime reference or its exports
//! never reach the static library.
//!
//! `src/ra8_camera_source_ceu.c` is deliberately NOT here: two host suites
//! (`tests/misc/src/test_ra8_camera.c`, `tests/misc/src/test_ra8_ceu_cov.c`)
//! white-box that file with `#include "ra8_camera_source_ceu.c"`, so it stays a
//! C translation unit and binds the same private vtable.

comptime {
    _ = @import("ra8_camera_abi.zig");
    _ = @import("source_memory.zig");
    _ = @import("codec_passthrough.zig");
    _ = @import("codec_jpeg_sw.zig");
}
