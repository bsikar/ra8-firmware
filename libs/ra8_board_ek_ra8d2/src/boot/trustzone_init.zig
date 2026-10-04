//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! EK-RA8D2 default TrustZone bring-up. [Ring 1 / Boot] {World: S}
//!
//! In an app built with TrustZone on (RA8_TRUSTZONE_ENABLE), the SAU is
//! programmed from the driver's own boot partition, `ra8_sau_apply_boot_map()`.
//! That call checks SAU_TYPE.SREGION first and programs nothing when the
//! silicon reports too few regions; the caller then sees SAU_CTRL.ENABLE clear
//! and falls back to the single-world model, so its status needs no handling
//! here. It touches no `.data` or `.bss`, which keeps it safe on the
//! pre-init reset path. With TrustZone off the function does nothing.
//!
//! The ns_usb_handoff and secure_only boot profiles and app-local copies
//! still replace this unit (cmake/ra8_app/sources.cmake, RA8FW-616). Built as
//! its own object per TrustZone setting, which arrives as
//! `@import("boot_options")`. Ported from trustzone_init.c (RA8FW-622).

const boot_options = @import("boot_options");

extern fn ra8_sau_apply_boot_map() callconv(.c) u16;

fn trustzoneInit() callconv(.c) void {
    if (!boot_options.trust_zone) return;
    _ = ra8_sau_apply_boot_map();
}

comptime {
    @export(&trustzoneInit, .{ .name = "ra8_trustzone_init" });
}
