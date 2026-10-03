//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Root of the static library: every C ABI file `ra8_c6link` ships.
//!
//! The pump's ABI sits beside the main membrane rather than inside it because
//! it calls back into the C dispatcher, which the host test of the membrane
//! does not link.

comptime {
    _ = @import("ra8_c6link_abi.zig");
    _ = @import("ra8_c6link_pump_abi.zig");
}
