//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Root of the GENERAL ra8_core archive: the ported TUs that export ordinary
//! `ra8_*` names and so collide with nothing a host test already links.
//!
//! The freestanding primitives are deliberately NOT here; they are their own
//! archive (`src/freestanding_root.zig`) because they export bare libc names.
//! Referencing each membrane is what pulls its exports into the archive.

comptime {
    _ = @import("pin_validator_abi");
    _ = @import("systick_abi");
    _ = @import("time_interface_systick_abi");
    _ = @import("time_abi");
    _ = @import("log_abi");
    _ = @import("decomp_abi");
    _ = @import("scb_abi");
    _ = @import("exception_abi");
    _ = @import("crashlog_abi");
    _ = @import("error_handler_abi");
    _ = @import("error_sink_abi");
    _ = @import("infrastructure_abi");
    _ = @import("sbrk_trap_abi");
}
