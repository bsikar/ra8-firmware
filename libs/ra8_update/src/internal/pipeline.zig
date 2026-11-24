//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Declarative update-pipeline state machine and stage engine shared by
//! OTA (`ra8_ota`) and DFU (`ra8_dfu` / `dfu_bootloader`).
//!
//! The module is pure Zig and dependency-inverted: callers supply storage,
//! crypto, anti-rollback, and commit/confirm/rollback hooks, so both transports
//! share identical staging bounds, constant-time digest checks, default-deny
//! RoT/rollback gates, and trial-vs-confirmed activation transitions.

const std = @import("std");
const Err = @import("err").Err;

pub const sha256_bytes: usize = 32;
pub const sig_max_bytes: usize = 96;
pub const page_bytes: u32 = 32;
pub const chunk_bytes: u32 = 4096;
pub const default_max_bytes: u32 = 0x80000;
pub const max_meta_bytes: u32 = 512;
pub const default_trial_budget: u8 = 3;

/// `ra8_update_transport_t`.
pub const Transport = enum(u8) {
    ota = 0,
    dfu = 1,
    mem = 2,

    pub fn fromRaw(raw: u8) ?Transport {
        return switch (raw) {
            0 => .ota,
            1 => .dfu,
            2 => .mem,
            else => null,
        };
    }
};

/// `ra8_update_payload_t`.
pub const PayloadKind = enum(u8) {
    bundle = 0,
    manifest_raw = 1,
    rot1_slot = 2,

    pub fn fromRaw(raw: u8) ?PayloadKind {
        return switch (raw) {
            0 => .bundle,
            1 => .manifest_raw,
            2 => .rot1_slot,
            else => null,
        };
    }
};

/// `ra8_update_apply_t`.
pub const ApplyMode = enum(u8) {
    ab_slots = 0,
    ab_banks = 1,
    staged = 2,

    pub fn fromRaw(raw: u8) ?ApplyMode {
        return switch (raw) {
            0 => .ab_slots,
            1 => .ab_banks,
            2 => .staged,
            else => null,
        };
    }
};

/// `ra8_update_stage_t`.
pub const Stage = enum(u8) {
    idle = 0,
    preflight = 1,
    staging = 2,
    staged = 3,
    verifying = 4,
    verified = 5,
    committing = 6,
    trial_pending = 7,
    confirmed = 8,
    rolled_back = 9,
    failed = 10,

    pub fn fromRaw(raw: u8) ?Stage {
        return switch (raw) {
            0 => .idle,
            1 => .preflight,
            2 => .staging,
            3 => .staged,
            4 => .verifying,
            5 => .verified,
            6 => .committing,
            7 => .trial_pending,
            8 => .confirmed,
            9 => .rolled_back,
            10 => .failed,
            else => null,
        };
    }
};

/// `ra8_update_pipeline_decl_t`.
pub const PipelineDecl = extern struct {
    transport: u8,
    payload: u8,
    apply_mode: u8,
    trial_budget: u8,
    enforce_rot: bool,
    enforce_rollback: bool,
    defer_floor_bump: bool,
    reserved0: u8 = 0,
};

comptime {
    std.debug.assert(@sizeOf(PipelineDecl) == 8);
    std.debug.assert(@offsetOf(PipelineDecl, "transport") == 0);
    std.debug.assert(@offsetOf(PipelineDecl, "payload") == 1);
    std.debug.assert(@offsetOf(PipelineDecl, "apply_mode") == 2);
    std.debug.assert(@offsetOf(PipelineDecl, "trial_budget") == 3);
    std.debug.assert(@offsetOf(PipelineDecl, "enforce_rot") == 4);
    std.debug.assert(@offsetOf(PipelineDecl, "enforce_rollback") == 5);
    std.debug.assert(@offsetOf(PipelineDecl, "defer_floor_bump") == 6);
    std.debug.assert(@offsetOf(PipelineDecl, "reserved0") == 7);
}

/// Canonical pipeline declaration for each transport family.
pub fn canonicalDecl(t: Transport) PipelineDecl {
    return switch (t) {
        .ota => .{
            .transport = @backingInt(Transport.ota),
            .payload = @backingInt(PayloadKind.bundle),
            .apply_mode = @backingInt(ApplyMode.ab_banks),
            .trial_budget = 0,
            .enforce_rot = true,
            .enforce_rollback = true,
            .defer_floor_bump = false,
            .reserved0 = 0,
        },
        .dfu => .{
            .transport = @backingInt(Transport.dfu),
            .payload = @backingInt(PayloadKind.bundle),
            .apply_mode = @backingInt(ApplyMode.ab_slots),
            .trial_budget = default_trial_budget,
            .enforce_rot = true,
            .enforce_rollback = true,
            .defer_floor_bump = true,
            .reserved0 = 0,
        },
        .mem => .{
            .transport = @backingInt(Transport.mem),
            .payload = @backingInt(PayloadKind.bundle),
            .apply_mode = @backingInt(ApplyMode.staged),
            .trial_budget = 0,
            .enforce_rot = true,
            .enforce_rollback = true,
            .defer_floor_bump = false,
            .reserved0 = 0,
        },
    };
}

/// Validate declarative pipeline invariants before any stage opens.
pub fn validateDecl(decl: *const PipelineDecl) u16 {
    if (Transport.fromRaw(decl.transport) == null) return Err.invalid_arg.raw();
    if (PayloadKind.fromRaw(decl.payload) == null) return Err.invalid_arg.raw();
    if (ApplyMode.fromRaw(decl.apply_mode) == null) return Err.invalid_arg.raw();
    if (decl.reserved0 != 0) return Err.invalid_arg.raw();
    // Deferring floor bump requires rollback tracking to stay enabled, and
    // non-zero trial budgets always defer floor advancement until confirm.
    if (decl.defer_floor_bump and !decl.enforce_rollback) return Err.invalid_arg.raw();
    if (decl.trial_budget > 0 and decl.enforce_rollback and !decl.defer_floor_bump) {
        return Err.invalid_arg.raw();
    }
    return Err.ok.raw();
}

/// Whether `s` represents a resting end state for the current update cycle.
pub fn stageIsTerminal(s: Stage) bool {
    return s == .confirmed or s == .rolled_back or s == .failed;
}

/// Constant-time comparison over two equal-length byte slices.
pub fn equalConstantTime(a: []const u8, b: []const u8) bool {
    if (a.len == 0 or a.len != b.len) return false;
    var diff: u8 = 0;
    for (a, b) |lhs, rhs| {
        diff |= lhs ^ rhs;
    }
    return diff == 0;
}

/// Validate a staging write span against total image length and alignment.
pub fn stageWriteValid(
    offset: u32,
    len: u32,
    bytes_written: u32,
    total_bytes: u32,
    align_bytes: u32,
) u16 {
    if (len == 0) return Err.invalid_size.raw();
    if (offset != bytes_written) return Err.invalid_arg.raw();
    const end: u64 = @as(u64, offset) + @as(u64, len);
    if (end > @as(u64, total_bytes)) return Err.invalid_size.raw();
    if (align_bytes > 1) {
        if ((offset % align_bytes) != 0) return Err.invalid_arg.raw();
        const is_final = end == @as(u64, total_bytes);
        if (!is_final and (len % align_bytes) != 0) return Err.invalid_size.raw();
    }
    return Err.ok.raw();
}

/// Validate total payload size before opening staging.
pub fn beginSizeValid(total_bytes: u32, max_bytes: u32, align_bytes: u32) u16 {
    if (total_bytes == 0) return Err.invalid_size.raw();
    if (max_bytes > 0 and total_bytes > max_bytes) return Err.invalid_size.raw();
    if (align_bytes > 1 and (total_bytes % align_bytes) != 0) return Err.invalid_size.raw();
    return Err.ok.raw();
}

/// Determine whether rollback floor commits during activation or waits for confirm.
pub fn shouldCommitFloorAtCommit(decl: *const PipelineDecl) bool {
    return decl.enforce_rollback and !decl.defer_floor_bump and decl.trial_budget == 0;
}
