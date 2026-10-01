//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for `libs/ra8_core/inc/ra8_error_handler.h`.
//!
//! `ra8_fatal_error` keeps WEAK linkage, which the C spelled `[[gnu::weak]]`.
//! That is the documented override seam: `ra8_check.h` tells field builds to
//! replace this symbol to add a watchdog reset or a safety-halt sequence, so
//! a strong definition here would turn every one of those links into a
//! duplicate-symbol error.
//!
//! The order below is the contract, not an implementation detail. Interrupts
//! are masked before anything else runs, and the two log calls come after,
//! best-effort: if the log backend is itself broken the halt still happens.
//! Logging must never be able to prevent the halt.

const fatal = @import("error_fatal");

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;
extern fn ra8_log_emit_error_val(tag: [*:0]const u8, message: [*:0]const u8, value: u32) void;

/// The companion label the C wrote in front of the numeric code.
const err_label: [*:0]const u8 = "err=";

fn fatalError(tag: [*:0]const u8, message: [*:0]const u8, err: u32) callconv(.c) noreturn {
    fatal.maskInterrupts();

    ra8_log_emit_error(tag, message);
    ra8_log_emit_error_val(tag, err_label, err);

    fatal.breakpoint();
    fatal.park();
}

comptime {
    @export(&fatalError, .{ .name = "ra8_fatal_error", .linkage = .weak });
}
