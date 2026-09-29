//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Archive root for `ra8_audio`. The facade and both backends live in
//! separate files, so each needs an explicit comptime reference or its
//! exports never reach the static library.

comptime {
    _ = @import("ra8_audio_abi.zig");
    _ = @import("source_memory.zig");
    _ = @import("source_pdm.zig");
}
