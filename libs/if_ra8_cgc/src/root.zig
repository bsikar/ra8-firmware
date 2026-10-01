//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Archive root for `if_ra8_cgc`, the RA8 chip adapter behind the neutral
//! `fw_if_clock` port. Referencing both membranes is what keeps their exports
//! in the archive.

comptime {
    _ = @import("clock_map_abi");
    _ = @import("clock_ops_abi");
}
