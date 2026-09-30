//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for `libs/ra8_core/inc/ra8_exception.h` (#2868).
//!
//! This is the fault path itself, so the ORDER below is the contract, not a
//! style choice:
//!
//!   1. Everything goes into the fixed-SRAM snapshot FIRST, before any call
//!      that could itself fault, and `magic` is written last. A secondary
//!      fault inside the log backend then costs the log line, never the
//!      record.
//!   2. The persistence hook runs next, while the snapshot is complete and
//!      nothing risky has run yet, so a field unit's post-mortem survives
//!      the coming reset (see `crashlog_abi.zig`).
//!   3. Logging is best effort and strictly after both.
//!   4. The halt is last, and on target it is the named `wfi` loop rather
//!      than `ra8_fatal_error`: that path issues `bkpt #0`, which on a board
//!      with no debugger attached re-enters HardFault and escalates to
//!      LOCKUP at PC=0xEFFFFFFE. Host builds keep the overridable fatal hook
//!      so `tests/misc/src/test_ra8_exception.c` can longjmp out.
//!
//! Every write to the snapshot goes through a volatile pointer, which is
//! what stops the compiler folding the invalidate-then-validate window that
//! ordering depends on.

const halt = @import("fault_halt");
const record = @import("fault_record");
const scb = @import("fault_scb");

const tag: [*:0]const u8 = "EXC";

extern fn ra8_log_emit_error_val(tag: [*:0]const u8, message: [*:0]const u8, value: u32) void;
extern fn ra8_fatal_error(tag: [*:0]const u8, message: [*:0]const u8, err: u32) callconv(.c) noreturn;

/// The post-decode persistence sink, `ra8_exception_persist_fn`.
const PersistFn = *const fn (decoded: *const volatile record.Last) callconv(.c) void;

/// The one fixed-SRAM snapshot, at a linker-stable address a debugger reads
/// by name with zero code running.
pub export var g_ra8_exception_last: record.Last = .{
    .magic = 0,
    .exc_number = 0,
    .frame = .{ .r0 = 0, .r1 = 0, .r2 = 0, .r3 = 0, .r12 = 0, .lr = 0, .pc = 0, .xpsr = 0 },
    .diag = .{ .cfsr = 0, .hfsr = 0, .dfsr = 0, .bfar = 0, .mmfar = 0, .afsr = 0, .sfsr = 0, .sfar = 0 },
    .frame_ptr = 0,
    .nmisr = 0,
};

/// The snapshot as the fault path touches it: every access volatile, so the
/// write order survives optimisation.
const last: *volatile record.Last = &g_ra8_exception_last;

/// ICU cause staged by the NMI entry point for the common path to fold in.
///
/// `ra8_exception_report` owns the snapshot write order, so the NMI entry
/// cannot write the record directly without racing it. A plain SRAM store
/// here cannot fault; the common path copies it into the record and clears
/// the stage so a later non-NMI record never inherits a stale cause.
var nmi_stage: u32 = 0;
const nmi_stage_cell: *volatile u32 = &nmi_stage;

/// Registered sink, or null when disarmed. Lives in `.bss`, zeroed by every
/// reset, so a consumer re-arms it early on each boot. Left null unless an
/// app opts into persistence, so the default fault path pulls in no
/// crash-log code.
var persist: ?PersistFn = null;

pub export fn ra8_exception_set_persist_hook(hook: ?PersistFn) callconv(.c) void {
    persist = hook;
}

pub export fn ra8_exception_capture_diagnostics(out: ?*record.Diagnostics) callconv(.c) void {
    // A null argument is tolerated and returns silently: the fault path must
    // never log from here.
    const slot = out orelse return;
    const fs = scb.readFaultStatus();
    slot.* = record.diagnosticsFrom(
        fs.cfsr,
        fs.hfsr,
        fs.dfsr,
        fs.mmfar,
        fs.bfar,
        fs.afsr,
        fs.sfsr,
        fs.sfar,
    );
}

/// Best-effort dump of a captured record. Runs strictly after the snapshot
/// is complete, so a secondary fault in the log backend can no longer lose
/// the record. On the default ITM backend a fault context drops every byte;
/// a registered byte sink still emits.
fn logFaultDump(
    frame: ?*const record.Frame,
    exc_number: u32,
    diag: *const record.Diagnostics,
    nmisr: u32,
) void {
    ra8_log_emit_error_val(tag, "exception", exc_number);

    if (frame) |f| {
        ra8_log_emit_error_val(tag, "pc  ", f.pc);
        ra8_log_emit_error_val(tag, "lr  ", f.lr);
        ra8_log_emit_error_val(tag, "xpsr", f.xpsr);
        ra8_log_emit_error_val(tag, "r0  ", f.r0);
        ra8_log_emit_error_val(tag, "r1  ", f.r1);
        ra8_log_emit_error_val(tag, "r2  ", f.r2);
        ra8_log_emit_error_val(tag, "r3  ", f.r3);
        ra8_log_emit_error_val(tag, "r12 ", f.r12);
    }

    ra8_log_emit_error_val(tag, "cfsr ", diag.cfsr);
    ra8_log_emit_error_val(tag, "hfsr ", diag.hfsr);
    ra8_log_emit_error_val(tag, "bfar ", diag.bfar);
    ra8_log_emit_error_val(tag, "mmfar", diag.mmfar);
    ra8_log_emit_error_val(tag, "sfsr ", diag.sfsr);
    ra8_log_emit_error_val(tag, "sfar ", diag.sfar);
    if (exc_number == record.exc.nmi) {
        ra8_log_emit_error_val(tag, "nmisr", nmisr);
    }
}

/// Step 1: capture EVERYTHING into fixed SRAM before any call that might
/// itself fault, so a debugger can still recover the original fault context
/// even if the log backend, ITM, or anything else takes a secondary fault.
fn snapshot(frame: ?*const record.Frame, exc_number: u32) record.Diagnostics {
    last.magic = 0;
    last.exc_number = exc_number;
    last.frame_ptr = @intFromPtr(frame);
    if (frame) |f| last.frame = f.*;

    var diag: record.Diagnostics = undefined;
    ra8_exception_capture_diagnostics(&diag);
    last.diag = diag;

    last.nmisr = nmi_stage_cell.*;
    nmi_stage_cell.* = 0;
    last.magic = record.magic.valid;
    return diag;
}

pub export fn ra8_exception_report(
    frame: ?*const record.Frame,
    exc_number: u32,
) callconv(.c) noreturn {
    const diag = snapshot(frame, exc_number);

    // Step 1b: persist the completed snapshot across the coming reset, if a
    // sink is armed. A plain SRAM copy, so it is fault-context safe.
    if (persist) |hook| hook(last);

    // Step 2: best-effort logging, strictly after the snapshot.
    logFaultDump(frame, exc_number, &diag, last.nmisr);

    // Step 3: halt at a named symbol on target; through the overridable
    // fatal hook on host so unit tests can longjmp out.
    if (comptime halt.on_target) halt.spin();
    ra8_fatal_error(tag, "fault", exc_number);
}

pub export fn ra8_exception_report_nmi(
    frame: ?*const record.Frame,
    nmisr: u32,
) callconv(.c) noreturn {
    // Plain SRAM store first (it cannot fault), so even a secondary fault
    // inside the common path leaves the cause recoverable: the stage is
    // copied into the record before `magic` is set.
    nmi_stage_cell.* = nmisr;
    ra8_exception_report(frame, record.exc.nmi);
}
