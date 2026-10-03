//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Archive root for everything except the USB host driver.
//!
//! `root.zig` carries every membrane and is what an ARM app links: there the
//! host driver's `ra8_usb_host_*` seam is resolved by `ra8_hal`. Off target
//! that seam has no implementation, so the host suites
//! (`tests/misc/src/test_ra8_dfu_boot.c`, `test_ra8_dfu_launch.c` and
//! `test_ra8_dfu_program.c`) link this root instead: the boot, launch and
//! program exports, whose only externs the host HAL already provides.

comptime {
    _ = @import("dfu_boot_abi");
    _ = @import("launch_abi");
    _ = @import("program_abi");
}
