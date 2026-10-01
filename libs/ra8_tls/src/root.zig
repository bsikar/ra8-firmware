//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Archive root for `ra8_tls`. Referencing the ABI module is what makes the
//! ten exported `ra8_tls_*` entry points reachable to the linker.

pub const abi = @import("ra8_tls_abi.zig");

comptime {
    _ = abi;
}
