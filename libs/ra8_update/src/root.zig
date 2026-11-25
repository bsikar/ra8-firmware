//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Archive root for `libs/ra8_update`: exports the C ABI membrane.

comptime {
    _ = @import("update_abi");
    _ = @import("bundle_abi");
    _ = @import("reconcile_abi");
}
