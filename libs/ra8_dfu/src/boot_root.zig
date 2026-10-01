//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Archive root for the pure boot logic alone.
//!
//! `root.zig` carries both membranes and is what an ARM app links: there the
//! host driver's `ra8_usb_host_*` seam is resolved by `ra8_hal`. Off target
//! that seam has no implementation, so the MC/DC suite in
//! `tests/misc/src/test_ra8_dfu_boot.c` links this root instead: the same
//! exports, with nothing to bind but the decisions themselves.

comptime {
    _ = @import("dfu_boot_abi");
}
