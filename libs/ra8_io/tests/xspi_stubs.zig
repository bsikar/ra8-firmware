//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Link stubs for the OSPI NOR block device's HAL externs (RA8FW-1085).
//!
//! Every ra8_io test root links the whole archive, so each one pulls in the
//! xSPI backend, whose ra8_xspi_flash_* driver externs no test but the
//! dedicated xspi root defines. Tests that do not exercise the xspi unit
//! pull this in with `comptime { _ = @import("xspi_stubs.zig"); }`; the
//! dedicated xspi root defines its own recording stubs instead and must not
//! import this file.

export fn ra8_xspi_flash_read(_: u8, _: u32, _: ?[*]u8, _: u32) c_int {
    return 0;
}
export fn ra8_xspi_flash_program(_: u8, _: u32, _: ?[*]const u8, _: u32) c_int {
    return 0;
}
export fn ra8_xspi_flash_erase_sector(_: u8, _: u32) c_int {
    return 0;
}
