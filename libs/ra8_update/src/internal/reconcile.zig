//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Pure Zig desired-state boot-time companion firmware reconciler.
//! Zero heap allocation; persists CRC-32-protected retry/backoff state (`RA8R`)
//! via caller hooks so power loss at any point converges idempotently across boots.

const std = @import("std");
const Err = @import("err").Err;
const bundle = @import("bundle");

pub const nv_magic: u32 = 0x5241_3852; // "RA8R"
pub const nv_version: u16 = 1;
pub const max_retries_limit: u8 = 16;
pub const max_backoff_shift: u8 = 10;
pub const nv_crc_prefix_bytes: usize = 84;

/// `ra8_reconcile_state_t`.
pub const OverallState = enum(u8) {
    idle = 0,
    converged = 1,
    pending_retry = 2,
    safe_halted = 3,

    pub fn fromRaw(raw: u8) ?OverallState {
        return switch (raw) {
            0 => .idle,
            1 => .converged,
            2 => .pending_retry,
            3 => .safe_halted,
            else => null,
        };
    }
};

/// `ra8_reconcile_target_status_t`.
pub const TargetStatus = enum(u8) {
    idle = 0,
    converged = 1,
    retrying = 2,
    safe_halted = 3,

    pub fn fromRaw(raw: u8) ?TargetStatus {
        return switch (raw) {
            0 => .idle,
            1 => .converged,
            2 => .retrying,
            3 => .safe_halted,
            else => null,
        };
    }
};

/// `ra8_reconcile_policy_t` (12 bytes).
pub const Policy = extern struct {
    base_backoff_ms: u32,
    max_backoff_ms: u32,
    max_retries: u8,
    backoff_shift: u8,
    reprobe_after_apply: bool,
    reserved0: u8,
};

/// `ra8_reconcile_target_nv_t` (16 bytes).
pub const TargetNv = extern struct {
    target: u32,
    desired_version: u32,
    last_reported_version: u32,
    retries_used: u8,
    status: u8,
    reserved0: u16,
};

/// `ra8_reconcile_nv_state_t` (88 bytes).
pub const NvState = extern struct {
    magic: u32,
    format_version: u16,
    flags: u16,
    bundle_version: u32,
    seq: u32,
    overall_state: u8,
    target_count: u8,
    reserved: [2]u8,
    targets: [bundle.max_images]TargetNv,
    crc32: u32,
};

/// `ra8_reconcile_report_t` (28 bytes).
pub const Report = extern struct {
    bundle_version: u32,
    last_backoff_ms: u32,
    applies_attempted: u32,
    applies_succeeded: u32,
    overall_state: u8,
    companions_total: u8,
    companions_converged: u8,
    companions_pending: u8,
    halted_target_count: u8,
    nv_reset_applied: bool,
    reserved0: [2]u8,
    last_err: u16,
    reserved1: u16,
};

/// `ra8_reconcile_ops_t` (7 pointer slots).
pub const Ops = extern struct {
    nv_load: ?*const fn (?*anyopaque, *NvState) callconv(.c) u16,
    nv_save: ?*const fn (?*anyopaque, *const NvState) callconv(.c) u16,
    query_companion: ?*const fn (?*anyopaque, u32, *u32, ?*u32) callconv(.c) u16,
    apply_companion: ?*const fn (?*anyopaque, *const bundle.ImageDesc) callconv(.c) u16,
    backoff_delay: ?*const fn (?*anyopaque, u32, u8, u32) callconv(.c) u16,
    on_safe_state: ?*const fn (?*anyopaque, u32, *const NvState) callconv(.c) void,
    ctx: ?*anyopaque,
};

comptime {
    std.debug.assert(@sizeOf(Policy) == 12);
    std.debug.assert(@alignOf(Policy) == 4);
    std.debug.assert(@sizeOf(TargetNv) == 16);
    std.debug.assert(@sizeOf(NvState) == 88);
    std.debug.assert(@alignOf(NvState) == 4);
    std.debug.assert(@offsetOf(NvState, "targets") == 20);
    std.debug.assert(@offsetOf(NvState, "crc32") == 84);
    std.debug.assert(@sizeOf(Report) == 28);
    std.debug.assert(@alignOf(Report) == 4);
    std.debug.assert(@sizeOf(Ops) == @sizeOf(?*anyopaque) * 7);
}

/// Compute standard IEEE 802.3 CRC-32 over `[0..len)`.
pub fn ieeeCrc32(bytes: []const u8) u32 {
    var crc: u32 = 0xFFFF_FFFF;
    for (bytes) |b| {
        crc ^= b;
        var bit: u4 = 0;
        while (bit < 8) : (bit += 1) {
            const mask: u32 = if ((crc & 1) != 0) 0xEDB8_8320 else 0;
            crc = (crc >> 1) ^ mask;
        }
    }
    return ~crc;
}

fn stampCrc(state: *NvState) void {
    state.crc32 = ieeeCrc32(std.mem.asBytes(state)[0..nv_crc_prefix_bytes]);
}

/// Validate a persisted NV state block's checksum, header fields, and target records.
pub fn validateNv(state: *const NvState) u16 {
    const expected_crc = ieeeCrc32(std.mem.asBytes(state)[0..nv_crc_prefix_bytes]);
    if (state.crc32 != expected_crc) return Err.crc_mismatch.raw();

    if (state.magic != nv_magic) return Err.validation_failed.raw();
    if (state.format_version != nv_version) return Err.validation_failed.raw();
    if (state.flags != 0) return Err.validation_failed.raw();
    if (state.reserved[0] != 0 or state.reserved[1] != 0) return Err.validation_failed.raw();
    if (OverallState.fromRaw(state.overall_state) == null) return Err.validation_failed.raw();
    if (state.target_count > bundle.max_images) return Err.validation_failed.raw();

    var seen: u32 = 0;
    const count: usize = state.target_count;
    for (0..count) |i| {
        const t = state.targets[i];
        const tgt = bundle.Target.fromRaw(t.target) orelse return Err.validation_failed.raw();
        if (tgt == .ra8_app) return Err.validation_failed.raw();
        if (TargetStatus.fromRaw(t.status) == null) return Err.validation_failed.raw();
        if (t.reserved0 != 0) return Err.validation_failed.raw();
        const bit = @as(u32, 1) << @as(u5, @intCast(t.target));
        if ((seen & bit) != 0) return Err.validation_failed.raw();
        seen |= bit;
    }
    for (count..bundle.max_images) |i| {
        const t = state.targets[i];
        if (t.target != 0 or t.desired_version != 0 or t.last_reported_version != 0 or
            t.retries_used != 0 or t.status != 0 or t.reserved0 != 0)
        {
            return Err.validation_failed.raw();
        }
    }
    return Err.ok.raw();
}

/// Validate `"RA8U"` descriptors and extract ordered companion descriptors (`!= ra8_app`).
fn collectCompanions(
    descs: []const bundle.ImageDesc,
    out_companions: *[bundle.max_images]bundle.ImageDesc,
) error{ InvalidArg, InvalidSize }!u8 {
    if (descs.len == 0 or descs.len > bundle.max_images) return error.InvalidArg;
    var seen_targets: u32 = 0;
    var comp_count: u8 = 0;

    for (descs) |d| {
        const tgt = bundle.Target.fromRaw(d.target) orelse return error.InvalidArg;
        if (bundle.PayloadFmt.fromRaw(d.format) == null) return error.InvalidArg;
        if (d.reserved0 != 0 or d.reserved1 != 0) return error.InvalidArg;
        if (d.size == 0) return error.InvalidSize;

        const bit = @as(u32, 1) << @as(u5, @intCast(d.target));
        if ((seen_targets & bit) != 0) return error.InvalidArg;
        seen_targets |= bit;

        if (tgt != .ra8_app) {
            out_companions[comp_count] = d;
            comp_count += 1;
        }
    }
    return comp_count;
}

/// Initialize a CRC-32-stamped NV state block from `"RA8U"` bundle descriptors.
pub fn nvInit(
    bundle_version: u32,
    descs: []const bundle.ImageDesc,
    out_state: *NvState,
) u16 {
    var companions: [bundle.max_images]bundle.ImageDesc = undefined;
    const comp_count = collectCompanions(descs, &companions) catch |e| return bundle.mapParseErr(e);

    var state = std.mem.zeroes(NvState);
    state.magic = nv_magic;
    state.format_version = nv_version;
    state.bundle_version = bundle_version;
    state.overall_state = @backingInt(if (comp_count == 0) OverallState.converged else OverallState.idle);
    state.target_count = comp_count;
    for (0..comp_count) |i| {
        state.targets[i] = .{
            .target = companions[i].target,
            .desired_version = companions[i].image_version,
            .last_reported_version = 0,
            .retries_used = 0,
            .status = @backingInt(TargetStatus.idle),
            .reserved0 = 0,
        };
    }
    stampCrc(&state);
    out_state.* = state;
    return Err.ok.raw();
}

/// Validate policy and compute exponential backoff clamped to `policy.max_backoff_ms`.
pub fn computeBackoffMs(policy: *const Policy, retries_used: u8) error{InvalidArg}!u32 {
    if (policy.max_retries == 0 or policy.max_retries > max_retries_limit) return error.InvalidArg;
    if (policy.backoff_shift > max_backoff_shift) return error.InvalidArg;
    if (policy.reserved0 != 0) return error.InvalidArg;
    if (policy.base_backoff_ms > policy.max_backoff_ms) return error.InvalidArg;

    if (retries_used == 0 or policy.base_backoff_ms == 0) return 0;
    const shift: u6 = @intCast(@min(retries_used - 1, policy.backoff_shift));
    const shifted: u64 = @as(u64, policy.base_backoff_ms) << shift;
    return @intCast(@min(shifted, @as(u64, policy.max_backoff_ms)));
}

fn validateBundleHeader(hdr: *const bundle.Header) error{InvalidArg}!void {
    if (hdr.magic != bundle.magic) return error.InvalidArg;
    if (hdr.format_version != bundle.format_version) return error.InvalidArg;
    if (hdr.header_bytes != bundle.header_bytes) return error.InvalidArg;
    if (hdr.image_count == 0 or hdr.image_count > bundle.max_images) return error.InvalidArg;
    if (hdr.flags != 0 or hdr.reserved != 0) return error.InvalidArg;
}

fn nvMatchesCompanions(nv: *const NvState, bundle_version: u32, companions: []const bundle.ImageDesc) bool {
    if (nv.bundle_version != bundle_version) return false;
    if (nv.target_count != companions.len) return false;
    for (0..companions.len) |i| {
        if (nv.targets[i].target != companions[i].target) return false;
        if (nv.targets[i].desired_version != companions[i].image_version) return false;
    }
    return true;
}

fn buildReport(nv: *const NvState, applies_attempted: u32, applies_succeeded: u32, last_backoff_ms: u32, nv_reset: bool, last_err: u16) Report {
    var conv: u8 = 0;
    var pend: u8 = 0;
    var halted: u8 = 0;
    for (0..nv.target_count) |i| {
        const st = TargetStatus.fromRaw(nv.targets[i].status) orelse .idle;
        switch (st) {
            .converged => conv += 1,
            .safe_halted => halted += 1,
            .idle, .retrying => pend += 1,
        }
    }
    return .{
        .bundle_version = nv.bundle_version,
        .last_backoff_ms = last_backoff_ms,
        .applies_attempted = applies_attempted,
        .applies_succeeded = applies_succeeded,
        .overall_state = nv.overall_state,
        .companions_total = nv.target_count,
        .companions_converged = conv,
        .companions_pending = pend,
        .halted_target_count = halted,
        .nv_reset_applied = nv_reset,
        .reserved0 = .{ 0, 0 },
        .last_err = last_err,
        .reserved1 = 0,
    };
}

fn finalizeStateSummary(nv: *NvState) void {
    var all_conv = true;
    var any_halted = false;
    for (0..nv.target_count) |i| {
        const st = TargetStatus.fromRaw(nv.targets[i].status) orelse .idle;
        if (st != .converged) all_conv = false;
        if (st == .safe_halted) any_halted = true;
    }
    if (any_halted) {
        nv.overall_state = @backingInt(OverallState.safe_halted);
    } else if (all_conv) {
        nv.overall_state = @backingInt(OverallState.converged);
    } else {
        nv.overall_state = @backingInt(OverallState.pending_retry);
    }
}

fn persistState(ops: *const Ops, nv: *NvState) u16 {
    nv.seq +%= 1;
    stampCrc(nv);
    return ops.nv_save.?(ops.ctx, nv);
}

fn enterSafeState(
    ops: *const Ops,
    nv: *NvState,
    idx: usize,
    failed_target: u32,
) u16 {
    nv.targets[idx].status = @backingInt(TargetStatus.safe_halted);
    nv.overall_state = @backingInt(OverallState.safe_halted);
    const save_rc = persistState(ops, nv);
    if (save_rc != Err.ok.raw()) return save_rc;
    if (ops.on_safe_state) |cb| {
        cb(ops.ctx, failed_target, nv);
    }
    return Err.ok.raw();
}

/// Execute one desired-state boot-time companion reconciliation pass.
pub fn boot(
    bundle_hdr: *const bundle.Header,
    descs_ptr: [*]const bundle.ImageDesc,
    policy: *const Policy,
    ops: *const Ops,
    out_report: *Report,
) u16 {
    const nv_load = ops.nv_load orelse return Err.null_ptr.raw();
    const nv_save = ops.nv_save orelse return Err.null_ptr.raw();
    const query_fn = ops.query_companion orelse return Err.null_ptr.raw();
    const apply_fn = ops.apply_companion orelse return Err.null_ptr.raw();
    _ = nv_save;

    validateBundleHeader(bundle_hdr) catch |e| return bundle.mapParseErr(e);
    _ = computeBackoffMs(policy, 0) catch return Err.invalid_arg.raw();

    const descs = descs_ptr[0..bundle_hdr.image_count];
    var companions: [bundle.max_images]bundle.ImageDesc = undefined;
    const comp_count = collectCompanions(descs, &companions) catch |e| return bundle.mapParseErr(e);

    var nv: NvState = undefined;
    var nv_reset = false;
    var dirty = false;

    const load_rc = nv_load(ops.ctx, &nv);
    if (load_rc == Err.ok.raw()) {
        const val_rc = validateNv(&nv);
        if (val_rc != Err.ok.raw() or !nvMatchesCompanions(&nv, bundle_hdr.bundle_version, companions[0..comp_count])) {
            nv_reset = true;
        }
    } else if (load_rc == Err.not_found.raw() or load_rc == Err.crc_mismatch.raw() or load_rc == Err.validation_failed.raw()) {
        nv_reset = true;
    } else {
        return load_rc;
    }

    if (nv_reset) {
        const init_rc = nvInit(bundle_hdr.bundle_version, descs, &nv);
        if (init_rc != Err.ok.raw()) return init_rc;
        dirty = true;
    }

    if (nv.overall_state == @backingInt(OverallState.safe_halted)) {
        out_report.* = buildReport(&nv, 0, 0, 0, nv_reset, Err.ok.raw());
        return Err.ok.raw();
    }

    var applies_attempted: u32 = 0;
    var applies_succeeded: u32 = 0;
    var last_backoff_ms: u32 = 0;
    var last_err: u16 = Err.ok.raw();

    for (0..comp_count) |i| {
        const desc = companions[i];
        var rep_ver: u32 = 0;
        var rep_proto: u32 = 0;
        const q_rc = query_fn(ops.ctx, desc.target, &rep_ver, &rep_proto);
        if (q_rc == Err.ok.raw()) {
            if (nv.targets[i].last_reported_version != rep_ver) {
                nv.targets[i].last_reported_version = rep_ver;
                dirty = true;
            }
            if (rep_ver == desc.image_version and bundle.compatProtoAllowed(&desc, rep_proto)) {
                if (nv.targets[i].status != @backingInt(TargetStatus.converged) or nv.targets[i].retries_used != 0) {
                    nv.targets[i].status = @backingInt(TargetStatus.converged);
                    nv.targets[i].retries_used = 0;
                    dirty = true;
                }
                continue;
            }
        } else {
            last_err = q_rc;
        }

        if (nv.targets[i].retries_used >= policy.max_retries) {
            const halt_rc = enterSafeState(ops, &nv, i, desc.target);
            if (halt_rc != Err.ok.raw()) return halt_rc;
            out_report.* = buildReport(&nv, applies_attempted, applies_succeeded, last_backoff_ms, nv_reset, last_err);
            return Err.ok.raw();
        }

        const delay_ms = computeBackoffMs(policy, nv.targets[i].retries_used) catch unreachable;
        last_backoff_ms = delay_ms;
        if (delay_ms > 0) {
            if (ops.backoff_delay) |delay_fn| {
                const d_rc = delay_fn(ops.ctx, desc.target, nv.targets[i].retries_used + 1, delay_ms);
                if (d_rc != Err.ok.raw()) return d_rc;
            }
        }

        // Persist retry increment prior to apply so mid-apply brownouts consume bounded budget.
        nv.targets[i].retries_used += 1;
        nv.targets[i].status = @backingInt(TargetStatus.retrying);
        nv.overall_state = @backingInt(OverallState.pending_retry);
        const pre_save_rc = persistState(ops, &nv);
        if (pre_save_rc != Err.ok.raw()) return pre_save_rc;
        dirty = false;

        applies_attempted += 1;
        const app_rc = apply_fn(ops.ctx, &desc);
        if (app_rc != Err.ok.raw()) {
            last_err = app_rc;
            if (nv.targets[i].retries_used >= policy.max_retries) {
                const halt_rc = enterSafeState(ops, &nv, i, desc.target);
                if (halt_rc != Err.ok.raw()) return halt_rc;
            }
            out_report.* = buildReport(&nv, applies_attempted, applies_succeeded, last_backoff_ms, nv_reset, last_err);
            return Err.ok.raw();
        }
        applies_succeeded += 1;

        if (policy.reprobe_after_apply) {
            var post_ver: u32 = 0;
            var post_proto: u32 = 0;
            const pq_rc = query_fn(ops.ctx, desc.target, &post_ver, &post_proto);
            if (pq_rc == Err.ok.raw()) {
                nv.targets[i].last_reported_version = post_ver;
                if (post_ver == desc.image_version and bundle.compatProtoAllowed(&desc, post_proto)) {
                    nv.targets[i].status = @backingInt(TargetStatus.converged);
                    nv.targets[i].retries_used = 0;
                    dirty = true;
                    continue;
                }
            } else {
                last_err = pq_rc;
            }
            if (nv.targets[i].retries_used >= policy.max_retries) {
                const halt_rc = enterSafeState(ops, &nv, i, desc.target);
                if (halt_rc != Err.ok.raw()) return halt_rc;
                out_report.* = buildReport(&nv, applies_attempted, applies_succeeded, last_backoff_ms, nv_reset, last_err);
                return Err.ok.raw();
            }
            dirty = true;
        }
    }

    const prev_overall = nv.overall_state;
    finalizeStateSummary(&nv);
    if (nv.overall_state != prev_overall) dirty = true;

    if (dirty) {
        const final_save_rc = persistState(ops, &nv);
        if (final_save_rc != Err.ok.raw()) return final_save_rc;
    }

    out_report.* = buildReport(&nv, applies_attempted, applies_succeeded, last_backoff_ms, nv_reset, last_err);
    return Err.ok.raw();
}
