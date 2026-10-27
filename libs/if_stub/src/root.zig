//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Archive root for `if_stub`, the proof binding behind the neutral `fw_if_*`
//! ports. Referencing both membranes is what keeps their exports in the
//! archive.

comptime {
    _ = @import("clock_stub_abi");
    _ = @import("clock_stub_ops_abi");
}
