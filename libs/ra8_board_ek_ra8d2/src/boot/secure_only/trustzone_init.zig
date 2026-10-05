//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! No-op TrustZone bring-up for the secure-only USB experiment.
//! [Ring 1 / Boot] {World: S}
//!
//! The dual-world `usb_cdc_echo` build programs the SAU to carve a Non-Secure
//! half out of MRAM/SRAM/SDRAM. On EK-RA8D2 silicon that build hangs in
//! `ra8_cgc_pll2_enable`, because the PRCR-protected PLL2/USB clock registers
//! (HUM Ch. 9) drop writes from the aliases the SAU labels Non-Secure.
//!
//! This profile touches no SAU or IDAU, so the chip stays in its reset-state
//! security configuration and every access from the image is Secure. If
//! `ra8_cgc_pll2_enable` succeeds here, the hang is a partitioning artifact
//! and not a hardware fault.
//!
//! Deliberately not the board default unit: that one takes its TrustZone
//! switch from the configure-wide RA8_TRUSTZONE_ENABLE and would program the
//! SAU in a configure that has it on. Selected by BOOT_PROFILE secure_only
//! (cmake/ra8_app/sources.cmake). Ported from trustzone_init.c (RA8FW-659).

fn trustzoneInit() callconv(.c) void {}

comptime {
    @export(&trustzoneInit, .{ .name = "ra8_trustzone_init" });
}
