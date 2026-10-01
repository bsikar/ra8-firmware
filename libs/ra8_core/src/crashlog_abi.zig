//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for `libs/ra8_core/inc/ra8_crashlog.h`.
//!
//! Owns the one record instance and where it lives; the state machine over
//! it is `internal/fault/crashlog.zig`.
//!
//! On the firmware build the record sits in `.noinit`, which the linker
//! script pins at the top of SRAM above the stack, so `Reset_Handler`'s
//! `.bss` zero-fill never touches it and a warm or watchdog reset leaves it
//! intact. On the host test build there is no reset to survive in a single
//! process, and Mach-O rejects a bare section name, so it is an ordinary
//! zero-initialised static.

const builtin = @import("builtin");

const crashlog = @import("fault_crashlog");
const record = @import("fault_record");

const on_target = builtin.target.os.tag == .freestanding;

/// The one cross-reset record. `.noinit` on an image, plain zeroed storage
/// on a host test binary.
const storage = if (on_target) struct {
    pub var cell: crashlog.Record linksection(".noinit") = undefined;
} else struct {
    pub var cell: crashlog.Record = std.mem.zeroes(crashlog.Record);
    const std = @import("std");
};

/// Reached only through a volatile pointer: the write order is the integrity
/// model and must not be folded away.
const rec: *volatile crashlog.Record = &storage.cell;

extern fn ra8_exception_set_persist_hook(
    hook: ?*const fn (decoded: *const volatile record.Last) callconv(.c) void,
) void;

pub export fn ra8_crashlog_install() callconv(.c) void {
    ra8_exception_set_persist_hook(&ra8_crashlog_record_fault);
}

pub export fn ra8_crashlog_record_fault(
    decoded: ?*const volatile record.Last,
) callconv(.c) void {
    const snapshot = decoded orelse return;
    crashlog.recordFault(rec, snapshot);
}

pub export fn ra8_crashlog_peek(out: ?*crashlog.Record) callconv(.c) bool {
    const slot = out orelse return false;
    return crashlog.peek(rec, slot);
}

pub export fn ra8_crashlog_claim() callconv(.c) void {
    crashlog.claim(rec);
}

pub export fn ra8_crashlog_safe_mode_requested() callconv(.c) bool {
    return crashlog.safeModeRequested(rec);
}

comptime {
    // Host-only test hooks, exactly as the C gated them on RA8_OFF_TARGET.
    if (!on_target) {
        @export(&testRecord, .{ .name = "ra8_crashlog_test_record" });
        @export(&testWipe, .{ .name = "ra8_crashlog_test_wipe" });
    }
}

fn testRecord() callconv(.c) *volatile crashlog.Record {
    return rec;
}

fn testWipe() callconv(.c) void {
    crashlog.wipe(rec);
}
