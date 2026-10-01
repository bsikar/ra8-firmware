//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Archive root for `ra8_dfu`. It exists to pull in the ABI membranes so
//! their `export`s land in the static library; the decisions are in
//! `internal/`.

comptime {
    _ = @import("dfu_host_abi");
    _ = @import("dfu_boot_abi");
}
