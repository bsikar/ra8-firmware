//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C membrane for anti-rollback: the four symbols
//! `inc/ra8_dfu_antirollback.h` declares, and the extra-MRAM-backed default
//! store behind them.
//!
//! The decisions are in `internal/antirollback.zig` and need no storage at
//! all. What is left here is the sequencing the launch gate owes its callers
//! (read the floor, apply the policy, persist the accepted version) with
//! every failure defaulting to deny, plus the one piece that genuinely has to
//! touch the device: reading a counter word that may never have been
//! programmed.

const builtin = @import("builtin");
const std = @import("std");
const ar = @import("antirollback");

/// Off target there is no extra-MRAM and no fault to recover from, so the
/// durable counter is a RAM shadow. On target both paths are real.
const off_target = builtin.target.os.tag != .freestanding;

const tag: [*:0]const u8 = "ROLLBACK";

/// `ra8_err_t` codes this membrane returns. `ra8_err_t` is a C23
/// `enum : uint16_t`, so these are the ABI values.
const Err = struct {
    pub const ok: u16 = 0;
    pub const validation_failed: u16 = 0x501;
    pub const null_ptr: u16 = 0x504;
};

/// Cortex-M addresses the on-target probe needs.
const Scb = struct {
    pub const cfsr: usize = 0xE000_ED28;
};

/// Base of the extra-MRAM option-setting window, `k_ra8_flash_extra_start`
/// in `ra8_flash_regs.h` (HUM Ch 59.7.4.5 Table 59.15 p 3592). The durable
/// highest-accepted version is a single little-endian `u32` here.
const Nv = struct {
    pub const counter_addr: usize = 0x02E0_7600;
};

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;
extern fn ra8_flash_extra_mram_write(mram_addr: u32, src: [*]const u8, len: u32) u16;

/// Order the probe's accesses against the flag writes around them. The
/// instructions only exist on the target, and the host build of this archive
/// links the C suite, so the barrier compiles away there.
fn barrier() void {
    if (comptime !off_target) asm volatile ("dsb 0xF\n isb 0xF\n" ::: .{ .memory = true });
}

/// Mirrors `ra8_rot_antirollback_store_t`: two function pointers, either of
/// which a caller may leave null, which is why the membrane checks both.
const Store = extern struct {
    read: ?*const fn (out_min_version: ?*u32) callconv(.c) u16,
    commit: ?*const fn (new_version: u32) callconv(.c) u16,
};

// ---------------------------------------------------------------- durable --

/// Host-test RAM shadow of the extra-MRAM counter. The flash fake models the
/// MACI registers but not the extra-MRAM data side, so writes never round-trip
/// through the real window.
var fake_counter: u32 = ar.Nv.erased;

/// Set only while `probeCounter` reads the counter word. The board's
/// BusFault/HardFault handler routes to `ra8_rot_antirollback_on_probe_fault`,
/// which recovers the deliberate fault only when this is set.
export var g_ra8_rot_ar_probing: bool = false;

/// Set by `ra8_rot_antirollback_on_probe_fault` when a probe read faulted.
export var g_ra8_rot_ar_faulted: bool = false;

/// Fault-tolerant read of the durable counter word. A bench run proved that a
/// virgin word in the corrected window reads back erased without faulting
/// (the recorded fault was the phantom 0x27000000 address, since
/// corrected), so the catch is belt-and-braces for the counter's own location.
fn probeCounter() u32 {
    g_ra8_rot_ar_faulted = false;
    g_ra8_rot_ar_probing = true;
    barrier();
    const raw = @as(*const volatile u32, @ptrFromInt(Nv.counter_addr)).*;
    barrier();
    g_ra8_rot_ar_probing = false;
    return if (g_ra8_rot_ar_faulted) ar.Nv.erased else raw;
}

/// The raw counter word, however this build reaches it.
fn rawCounter() u32 {
    return if (off_target) fake_counter else probeCounter();
}

export fn ra8_rot_antirollback_on_probe_fault(exc_frame: ?[*]u32) bool {
    const frame = exc_frame orelse return false;
    if (!g_ra8_rot_ar_probing) return false;
    g_ra8_rot_ar_probing = false;
    g_ra8_rot_ar_faulted = true;

    // The basic exception frame is [R0 R1 R2 R3 R12 LR PC xPSR]; index 6 is
    // the stacked PC. Advance it past the faulting load so the exception
    // return resumes at the next instruction.
    const instr = @as(*const volatile u16, @ptrFromInt(@as(usize, frame[6]))).*;
    frame[6] += ar.instructionWidth(instr);

    // W1C-clear the sticky Configurable Fault Status so this deliberate fault
    // does not shadow a later real one: every bit that reads as 1 is written
    // back as 1, which clears it.
    const cfsr: *volatile u32 = @ptrFromInt(Scb.cfsr);
    cfsr.* = cfsr.*;
    barrier();
    return true;
}

// ------------------------------------------------------------ default store --

fn defaultRead(out_min_version: ?*u32) callconv(.c) u16 {
    const out = out_min_version orelse {
        ra8_log_emit_error(tag, "anti-rollback: out_min_version is NULL");
        return Err.null_ptr;
    };
    out.* = ar.storedFrom(rawCounter());
    return Err.ok;
}

fn defaultCommit(new_version: u32) callconv(.c) u16 {
    const stored = ar.storedFrom(rawCounter());
    if (!ar.needsCommit(new_version, stored)) return Err.ok;

    if (off_target) {
        fake_counter = new_version;
        return Err.ok;
    }
    // Extra-MRAM is bit-alterable, so the advance is a plain program with no
    // erase cycle. A write error propagates and the verify default-denies.
    var le: [4]u8 = undefined;
    std.mem.writeInt(u32, &le, new_version, .little);
    return ra8_flash_extra_mram_write(@intCast(Nv.counter_addr), &le, le.len);
}

const default_store: Store = .{ .read = defaultRead, .commit = defaultCommit };

// -------------------------------------------------------------- the exports --

export fn ra8_rot_antirollback_check(image_version: u32, stored_min_version: u32) u16 {
    if (!ar.accepts(image_version, stored_min_version)) {
        ra8_log_emit_error(tag, "anti-rollback: image version below stored minimum");
        return Err.validation_failed;
    }
    return Err.ok;
}

export fn ra8_rot_antirollback_verify(store: ?*const Store, image_version: u32) u16 {
    const s = store orelse {
        ra8_log_emit_error(tag, "anti-rollback: store is NULL");
        return Err.null_ptr;
    };
    const read = s.read orelse {
        ra8_log_emit_error(tag, "anti-rollback: store->read is NULL");
        return Err.null_ptr;
    };
    const commit = s.commit orelse {
        ra8_log_emit_error(tag, "anti-rollback: store->commit is NULL");
        return Err.null_ptr;
    };

    // A read fault is default-deny: without a floor there is nothing to
    // enforce the policy against.
    var stored_min: u32 = 0;
    const read_err = read(&stored_min);
    if (read_err != Err.ok) {
        ra8_log_emit_error(tag, "anti-rollback: stored-version read failed");
        return read_err;
    }

    const policy_err = ra8_rot_antirollback_check(image_version, stored_min);
    if (policy_err != Err.ok) {
        ra8_log_emit_error(tag, "anti-rollback: downgrade rejected");
        return policy_err;
    }

    // Accepted: advance the durable counter. A commit fault is default-deny
    // too, so a floor that could not be written never launches.
    const commit_err = commit(image_version);
    if (commit_err != Err.ok) {
        ra8_log_emit_error(tag, "anti-rollback: counter commit failed");
        return commit_err;
    }
    return Err.ok;
}

export fn ra8_rot_antirollback_default_store() *const Store {
    return &default_store;
}
