//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Test-only link seam for the render-face port (RA8FW-1221). The Zig unit
//! harness runs without reflow_render.c, so priv_reflow_render_is_blank_cp
//! is answered here: U+FFFF counts as blank and nothing else does, which
//! lets the picker's blank guard be driven. The real blank policy lives in
//! reflow_render.c and is covered by the CMake suite. This file is imported
//! only by reflow_parse_tests.zig, never by the ABI module, and never ships.

export fn priv_reflow_render_is_blank_cp(cp: c_int) bool {
    return cp == 0xFFFF;
}
