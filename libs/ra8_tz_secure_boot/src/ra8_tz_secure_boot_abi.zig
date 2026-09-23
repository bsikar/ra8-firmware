//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for `libs/ra8_tz_secure_boot/inc/ra8_tz_secure_boot.h`. The
//! partition table, the field encodings and the guard predicates live in
//! `internal/root.zig`; this file owns the exported symbols, the register
//! access (real MMIO on a freestanding build, the documented captures on a
//! hosted one), the guards in their original order, the `ra8_err_t` mapping,
//! and the one diagnostic line per guard the C emitted.
//!
//! Two build options carry the C's preprocessor switches:
//!   * `off-target` is `RA8_OFF_TARGET`, defaulted from the target, and picks
//!     captures-plus-return over MMIO-plus-BLXNS.
//!   * `enable-root-of-trust` is `RA8_ENABLE_ROOT_OF_TRUST` and compiles the
//!     default-deny authentication gate in front of the BLXNS. It has to be
//!     passed to this build, because a `-D` on the app target no longer
//!     reaches a translation unit that is not compiled by CMake; see
//!     `cmake/ra8_app/zig_libs.cmake`.

const std = @import("std");
const build_config = @import("build_config");
const implementation = @import("internal/root.zig");

/// Boot progress marker (`ra8_tz_secure_boot_step_t`).
pub const Step = implementation.Step;
/// Host-side capture model, exposed for the Zig ABI tests.
pub const HostState = implementation.HostState;

// Representation checks for the public C ABI's byte-sized progress enum and
// fixed eight-byte Non-Secure RoT header. Keep these at the ABI membrane so
// the ABI policy can audit them alongside the exported declarations.
comptime {
    std.debug.assert(@sizeOf(Step) == 1);
    std.debug.assert(@sizeOf(implementation.NsRotHeader) == 8);
    std.debug.assert(@offsetOf(implementation.NsRotHeader, "magic") == 0);
    std.debug.assert(@offsetOf(implementation.NsRotHeader, "body_len") == 4);
}

/// Subset of `ra8_err_t` this library returns.
pub const TzError = enum(u16) {
    ok = 0,
    invalid_arg = 0x103,
    invalid_size = 0x105,
    not_supported = 0x107,
    validation_failed = 0x501,
    null_ptr = 0x504,
};

/// Component tag on the library's log lines, matching the C's `s_tag`.
const tag: [*:0]const u8 = "TZBOOT";

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;
extern fn ra8_log_emit_error_val(tag: [*:0]const u8, message: [*:0]const u8, value: u32) void;

/// The gate is compiled only where the C compiled it: a target build with
/// root of trust enabled. Declaring the two externs inside this struct keeps
/// them out of every other build's symbol table.
const rot_gate_enabled = !build_config.off_target and build_config.enable_root_of_trust;

const rot = if (rot_gate_enabled) struct {
    extern fn ra8_rot_trailer_after(image_base: ?*const anyopaque, body_len: u32) ?*const anyopaque;
    extern fn ra8_rot_verify_image(body: [*]const u8, body_len: u32, trailer: ?*const anyopaque) u16;
} else struct {};

/// Host captures exist only on a hosted build, exactly as the C's `s_host`
/// sat inside `#ifdef RA8_OFF_TARGET`.
const host = if (build_config.off_target) struct {
    var state: HostState = .{};
} else struct {};

/// Progress counter. Read through a volatile pointer so a bench SWD probe
/// still sees every milestone the C's `volatile` guaranteed.
var step_counter: Step = .idle;

inline fn setStep(value: Step) void {
    @as(*volatile Step, &step_counter).* = value;
}

inline fn currentStep() Step {
    return @as(*const volatile Step, &step_counter).*;
}

inline fn write32(addr: usize, value: u32) void {
    if (build_config.off_target) {
        host.state.write32(addr, value);
    } else {
        @as(*volatile u32, @ptrFromInt(addr)).* = value;
    }
}

inline fn read32(addr: usize) u32 {
    if (build_config.off_target) {
        return host.state.read32(addr);
    } else {
        return @as(*const volatile u32, @ptrFromInt(addr)).*;
    }
}

inline fn write16(addr: usize, value: u16) void {
    if (build_config.off_target) {
        host.state.write16(addr, value);
    } else {
        @as(*volatile u16, @ptrFromInt(addr)).* = value;
    }
}

// The C's host build compiled both barriers away entirely; only the target
// build emits them. `dsb sy` / `isb sy` are the assembler spellings of the
// C's `dsb 0xF` / `isb 0xF`: option 0xF is SY.
inline fn dsb() void {
    if (!build_config.off_target) asm volatile ("dsb sy" ::: "memory");
}

inline fn isb() void {
    if (!build_config.off_target) asm volatile ("isb sy" ::: "memory");
}

/// Programme one SAU region through RNR / RBAR / RLAR.
fn setRegion(region: u8, base: u32, limit: u32, is_nsc: bool) void {
    write32(implementation.Addr.sau_rnr, region);
    write32(implementation.Addr.sau_rbar, base);
    write32(implementation.Addr.sau_rlar, implementation.rlarFor(limit, is_nsc));
    if (build_config.off_target) {
        host.state.noteRegion(region, base, limit, is_nsc);
    }
}

export fn ra8_tz_secure_boot_sau_init() callconv(.c) u16 {
    const sau_type = read32(implementation.Addr.sau_type);
    if (!implementation.sauRegionsSufficient(sau_type)) {
        ra8_log_emit_error(tag, "SAU_TYPE.SREGION below required count");
        return @intFromEnum(TzError.not_supported);
    }

    for (implementation.partition) |spec| {
        setRegion(spec.index, spec.base, spec.limit, spec.is_nsc);
    }

    dsb();
    write32(implementation.Addr.sau_ctrl, implementation.sau_ctrl_enable);
    dsb();
    isb();

    setStep(.sau_done);
    return @intFromEnum(TzError.ok);
}

export fn ra8_tz_secure_boot_security_init(ipcsar_value: u32, ipcpar_value: u32) callconv(.c) u16 {
    // HUM Ch 13.2.1 "PRCR_S": open the PRC4 gate so the CPSCU writes land.
    write16(implementation.Addr.prcr_s, implementation.prcr_s_open);
    setStep(.prcr_unlocked);

    // HUM Ch 3.2.1 "IPCSAR" and Ch 3.2.2 "IPCPAR".
    write32(implementation.Addr.ipcsar, ipcsar_value);
    write32(implementation.Addr.ipcpar, ipcpar_value);
    setStep(.ipcsar_written);

    // HUM Ch 13.2.1 "PRCR_S": restore write protection.
    write16(implementation.Addr.prcr_s, implementation.prcr_s_close);
    setStep(.prcr_relocked);

    dsb();
    return @intFromEnum(TzError.ok);
}

export fn ra8_tz_ns_signed_body_len(ns_vector_table: ?[*]const u32) callconv(.c) u32 {
    const base = ns_vector_table orelse {
        ra8_log_emit_error(tag, "ns_vector_table is NULL");
        return 0;
    };

    const bytes: [*]const u8 = @ptrCast(base);
    const header: *const implementation.NsRotHeader =
        @ptrCast(@alignCast(bytes + implementation.ns_rot_header_offset));

    if (!implementation.headerMagicOk(header.magic)) {
        ra8_log_emit_error(tag, "NS RoT header magic mismatch");
        return 0;
    }
    return header.body_len;
}

/// Authenticate the NS image before the BLXNS, default-deny. Absent on a host
/// build and on a target build without root of trust, exactly as the C's
/// `internal_ns_verify_or_deny` was.
fn nsVerifyOrDeny(ns_vector_table: ?[*]const u32) u16 {
    const base = ns_vector_table orelse {
        ra8_log_emit_error(tag, "ns_vector_table");
        return @intFromEnum(TzError.null_ptr);
    };

    if (!rot_gate_enabled) return @intFromEnum(TzError.ok);

    const body_len = ra8_tz_ns_signed_body_len(base);
    if (body_len == 0) {
        ra8_log_emit_error(tag, "NS RoT header missing/invalid -- denying BLXNS");
        return @intFromEnum(TzError.validation_failed);
    }

    const trailer = rot.ra8_rot_trailer_after(@ptrCast(base), body_len) orelse {
        ra8_log_emit_error(tag, "NS RoT body_len out of range -- denying BLXNS");
        return @intFromEnum(TzError.invalid_size);
    };

    return rot.ra8_rot_verify_image(@ptrCast(base), body_len, trailer);
}

export fn ra8_tz_secure_boot_jump_ns(ns_vector_table: ?[*]const u32) callconv(.c) u16 {
    const vector_table = ns_vector_table orelse {
        ra8_log_emit_error(tag, "ns_vector_table");
        return @intFromEnum(TzError.null_ptr);
    };

    if (!implementation.pointerAligned4(@intFromPtr(vector_table))) {
        ra8_log_emit_error(tag, "ns_vector_table misaligned");
        return @intFromEnum(TzError.invalid_arg);
    }

    const initial_sp = vector_table[0];
    const reset_entry = vector_table[1];

    if (implementation.resetEntryBogus(reset_entry)) {
        ra8_log_emit_error_val(tag, "NS reset vector invalid", reset_entry);
        return @intFromEnum(TzError.invalid_arg);
    }

    const auth = nsVerifyOrDeny(vector_table);
    if (auth != @intFromEnum(TzError.ok)) {
        ra8_log_emit_error(tag, "NS image authentication failed -- denying BLXNS");
        return auth;
    }

    // Armv8-M ARM B3.2.4: VTOR_NS reached through the 0xE002... alias.
    write32(implementation.Addr.scb_vtor_ns, @truncate(@intFromPtr(vector_table)));
    setStep(.blxns_armed);

    if (build_config.off_target) {
        host.state.blxns_target = reset_entry;
        host.state.blxns_msp_ns = initial_sp;
        setStep(.branched);
        return @intFromEnum(TzError.ok);
    } else {
        const ns_entry = implementation.nsEntryFromResetVector(reset_entry);
        asm volatile (
            \\msr msp_ns, %[stack]
            \\blxns %[entry]
            :
            : [stack] "r" (initial_sp),
              [entry] "r" (ns_entry),
            : "memory"
        );
        // Unreachable on target.
        return @intFromEnum(TzError.ok);
    }
}

export fn ra8_tz_secure_boot_run(
    ipcsar_value: u32,
    ipcpar_value: u32,
    ns_vector_table: ?[*]const u32,
) callconv(.c) u16 {
    if (ns_vector_table == null) {
        ra8_log_emit_error(tag, "ns_vector_table");
        return @intFromEnum(TzError.null_ptr);
    }

    const sau_err = ra8_tz_secure_boot_sau_init();
    if (sau_err != @intFromEnum(TzError.ok)) return sau_err;

    const security_err = ra8_tz_secure_boot_security_init(ipcsar_value, ipcpar_value);
    if (security_err != @intFromEnum(TzError.ok)) return security_err;

    return ra8_tz_secure_boot_jump_ns(ns_vector_table);
}

export fn ra8_tz_secure_boot_host_reset() callconv(.c) void {
    if (build_config.off_target) {
        host.state.reset();
        setStep(.idle);
    }
}

export fn ra8_tz_secure_boot_host_blxns_target() callconv(.c) u32 {
    if (build_config.off_target) return host.state.blxns_target;
    return 0;
}

export fn ra8_tz_secure_boot_get_step() callconv(.c) u8 {
    return @intFromEnum(currentStep());
}

/// Test-only view of the captures the exported symbols write.
pub fn testHostState() *HostState {
    comptime std.debug.assert(build_config.off_target);
    return &host.state;
}
