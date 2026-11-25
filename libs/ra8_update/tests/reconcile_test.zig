//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host power-cut fault-injection, idempotent convergence, bounded backoff, and
//! safe-state tests for the `ra8_reconcile` desired-state companion reconciler.

const std = @import("std");
const Err = @import("err").Err;
const bundle = @import("bundle");
const rec = @import("reconcile");

const FaultStage = enum {
    none,
    nv_load_hw_fail,
    nv_load_corrupt,
    query_fail_c6,
    pre_apply_nv_save_cut,
    apply_cut_before_flash_c6,
    apply_flashed_then_cut_c6,
    backoff_delay_cut,
    apply_cut_before_flash_disp,
    final_nv_save_cut,
};

const SimEnv = struct {
    has_nv: bool = false,
    nv: rec.NvState = std.mem.zeroes(rec.NvState),
    nv_save_calls: u32 = 0,
    c6_ver: u32 = 10,
    c6_proto: u32 = 4,
    disp_ver: u32 = 15,
    disp_proto: u32 = 1,
    c6_applies: u32 = 0,
    disp_applies: u32 = 0,
    backoff_calls: u32 = 0,
    backoff_log: [16]u32 = @splat(0),
    safe_calls: u32 = 0,
    last_safe_target: u32 = 0,
    c6_never_converge: bool = false,
    apply_always_fail_c6: bool = false,
    fault: FaultStage = .none,
    commit_ver_on_apply: bool = true,
    save_calls_this_boot: u32 = 0,

    fn asCtx(self: *SimEnv) *anyopaque {
        return @ptrCast(self);
    }

    fn fromCtx(ctx_opt: ?*anyopaque) *SimEnv {
        return @ptrCast(@alignCast(ctx_opt.?));
    }

    fn hookNvLoad(ctx_opt: ?*anyopaque, out_state: *rec.NvState) callconv(.c) u16 {
        const self = fromCtx(ctx_opt);
        if (self.fault == .nv_load_hw_fail) {
            self.fault = .none;
            return Err.hw_error.raw();
        }
        if (self.fault == .nv_load_corrupt) {
            self.fault = .none;
            var corrupt = self.nv;
            corrupt.seq ^= 0x1;
            out_state.* = corrupt;
            return Err.ok.raw();
        }
        if (!self.has_nv) return Err.not_found.raw();
        out_state.* = self.nv;
        return Err.ok.raw();
    }

    fn hookNvSave(ctx_opt: ?*anyopaque, state: *const rec.NvState) callconv(.c) u16 {
        const self = fromCtx(ctx_opt);
        self.save_calls_this_boot += 1;
        if (self.fault == .pre_apply_nv_save_cut and state.overall_state == @backingInt(rec.OverallState.pending_retry)) {
            self.fault = .none;
            return Err.hw_error.raw();
        }
        if (self.fault == .final_nv_save_cut and state.overall_state == @backingInt(rec.OverallState.converged)) {
            self.fault = .none;
            return Err.hw_error.raw();
        }
        self.nv = state.*;
        self.has_nv = true;
        self.nv_save_calls += 1;
        return Err.ok.raw();
    }

    fn hookQuery(ctx_opt: ?*anyopaque, target_raw: u32, out_ver: *u32, out_proto_opt: ?*u32) callconv(.c) u16 {
        const self = fromCtx(ctx_opt);
        if (target_raw == @backingInt(bundle.Target.esp32_c6)) {
            if (self.fault == .query_fail_c6) {
                self.fault = .none;
                return Err.hw_error.raw();
            }
            out_ver.* = self.c6_ver;
            if (out_proto_opt) |p| p.* = self.c6_proto;
            return Err.ok.raw();
        }
        if (target_raw == @backingInt(bundle.Target.display)) {
            out_ver.* = self.disp_ver;
            if (out_proto_opt) |p| p.* = self.disp_proto;
            return Err.ok.raw();
        }
        return Err.not_found.raw();
    }

    fn hookApply(ctx_opt: ?*anyopaque, desc: *const bundle.ImageDesc) callconv(.c) u16 {
        const self = fromCtx(ctx_opt);
        if (desc.target == @backingInt(bundle.Target.esp32_c6)) {
            self.c6_applies += 1;
            if (self.apply_always_fail_c6) return Err.hw_error.raw();
            if (self.fault == .apply_cut_before_flash_c6) {
                self.fault = .none;
                return Err.hw_error.raw();
            }
            if (!self.c6_never_converge and self.commit_ver_on_apply) {
                self.c6_ver = desc.image_version;
                if (desc.compat_proto_ver != 0) self.c6_proto = desc.compat_proto_ver;
            }
            if (self.fault == .apply_flashed_then_cut_c6) {
                self.fault = .none;
                return Err.hw_error.raw();
            }
            return Err.ok.raw();
        }
        if (desc.target == @backingInt(bundle.Target.display)) {
            self.disp_applies += 1;
            if (self.fault == .apply_cut_before_flash_disp) {
                self.fault = .none;
                return Err.hw_error.raw();
            }
            if (self.commit_ver_on_apply) {
                self.disp_ver = desc.image_version;
                if (desc.compat_proto_ver != 0) self.disp_proto = desc.compat_proto_ver;
            }
            return Err.ok.raw();
        }
        return Err.invalid_arg.raw();
    }

    fn hookBackoff(ctx_opt: ?*anyopaque, target_raw: u32, attempt: u8, delay_ms: u32) callconv(.c) u16 {
        _ = target_raw;
        _ = attempt;
        const self = fromCtx(ctx_opt);
        if (self.backoff_calls < self.backoff_log.len) {
            self.backoff_log[self.backoff_calls] = delay_ms;
        }
        self.backoff_calls += 1;
        if (self.fault == .backoff_delay_cut) {
            self.fault = .none;
            return Err.hw_error.raw();
        }
        return Err.ok.raw();
    }

    fn hookSafeState(ctx_opt: ?*anyopaque, target_raw: u32, state: *const rec.NvState) callconv(.c) void {
        _ = state;
        const self = fromCtx(ctx_opt);
        self.safe_calls += 1;
        self.last_safe_target = target_raw;
    }

    fn ops(self: *SimEnv) rec.Ops {
        return .{
            .nv_load = hookNvLoad,
            .nv_save = hookNvSave,
            .query_companion = hookQuery,
            .apply_companion = hookApply,
            .backoff_delay = hookBackoff,
            .on_safe_state = hookSafeState,
            .ctx = self.asCtx(),
        };
    }
};

fn makeBundle(bundle_ver: u32, c6_ver: u32, disp_ver: u32, c6_proto: u32) struct {
    hdr: bundle.Header,
    descs: [3]bundle.ImageDesc,
} {
    const pay_off = bundle.expectedPayloadsOffset(3);
    const descs = [3]bundle.ImageDesc{
        .{
            .target = @backingInt(bundle.Target.ra8_app),
            .format = @backingInt(bundle.PayloadFmt.rot1_img),
            .flags = 0,
            .image_version = 50,
            .compat_proto_ver = c6_proto,
            .offset = pay_off,
            .size = 64,
            .reserved0 = 0,
            .reserved1 = 0,
            .sha256 = @splat(0x11),
        },
        .{
            .target = @backingInt(bundle.Target.esp32_c6),
            .format = @backingInt(bundle.PayloadFmt.c6_img),
            .flags = 0,
            .image_version = c6_ver,
            .compat_proto_ver = c6_proto,
            .offset = pay_off + 64,
            .size = 64,
            .reserved0 = 0,
            .reserved1 = 0,
            .sha256 = @splat(0x22),
        },
        .{
            .target = @backingInt(bundle.Target.display),
            .format = @backingInt(bundle.PayloadFmt.raw),
            .flags = 0,
            .image_version = disp_ver,
            .compat_proto_ver = 0,
            .offset = pay_off + 128,
            .size = 64,
            .reserved0 = 0,
            .reserved1 = 0,
            .sha256 = @splat(0x33),
        },
    };
    const hdr = bundle.Header{
        .magic = bundle.magic,
        .format_version = bundle.format_version,
        .header_bytes = bundle.header_bytes,
        .bundle_version = bundle_ver,
        .image_count = 3,
        .flags = 0,
        .total_bytes = pay_off + 192,
        .payloads_offset = pay_off,
        .sig_len = bundle.sig_slot_bytes,
        .reserved = 0,
    };
    return .{ .hdr = hdr, .descs = descs };
}

test "nvInit and validateNv build CRC-32-protected RA8R state and reject corruption" {
    const b = makeBundle(7, 20, 30, 4);
    var nv: rec.NvState = undefined;
    try std.testing.expectEqual(Err.ok.raw(), rec.nvInit(b.hdr.bundle_version, &b.descs, &nv));
    try std.testing.expectEqual(rec.nv_magic, nv.magic);
    try std.testing.expectEqual(@as(u8, 2), nv.target_count);
    try std.testing.expectEqual(@backingInt(bundle.Target.esp32_c6), nv.targets[0].target);
    try std.testing.expectEqual(@as(u32, 20), nv.targets[0].desired_version);
    try std.testing.expectEqual(@backingInt(bundle.Target.display), nv.targets[1].target);
    try std.testing.expectEqual(@as(u32, 30), nv.targets[1].desired_version);
    try std.testing.expectEqual(Err.ok.raw(), rec.validateNv(&nv));

    var bit_flip = nv;
    bit_flip.targets[0].retries_used = 1;
    try std.testing.expectEqual(Err.crc_mismatch.raw(), rec.validateNv(&bit_flip));

    var forged = nv;
    forged.magic = 0xDEAD_BEEF;
    forged.crc32 = rec.ieeeCrc32(std.mem.asBytes(&forged)[0..rec.nv_crc_prefix_bytes]);
    try std.testing.expectEqual(Err.validation_failed.raw(), rec.validateNv(&forged));
}

test "computeBackoffMs enforces bounded exponential clamping and policy guards" {
    const p = rec.Policy{
        .base_backoff_ms = 100,
        .max_backoff_ms = 350,
        .max_retries = 5,
        .backoff_shift = 2,
        .reprobe_after_apply = true,
        .reserved0 = 0,
    };
    try std.testing.expectEqual(@as(u32, 0), try rec.computeBackoffMs(&p, 0));
    try std.testing.expectEqual(@as(u32, 100), try rec.computeBackoffMs(&p, 1));
    try std.testing.expectEqual(@as(u32, 200), try rec.computeBackoffMs(&p, 2));
    try std.testing.expectEqual(@as(u32, 350), try rec.computeBackoffMs(&p, 3));
    try std.testing.expectEqual(@as(u32, 350), try rec.computeBackoffMs(&p, 4));

    var bad = p;
    bad.max_retries = 0;
    try std.testing.expectError(error.InvalidArg, rec.computeBackoffMs(&bad, 1));
    bad = p;
    bad.base_backoff_ms = 500;
    try std.testing.expectError(error.InvalidArg, rec.computeBackoffMs(&bad, 1));
    bad = p;
    bad.backoff_shift = 11;
    try std.testing.expectError(error.InvalidArg, rec.computeBackoffMs(&bad, 1));
}

test "power cuts at every step of multi-image apply still converge idempotently" {
    const b = makeBundle(10, 20, 30, 4);
    const p = rec.Policy{
        .base_backoff_ms = 50,
        .max_backoff_ms = 400,
        .max_retries = 6,
        .backoff_shift = 3,
        .reprobe_after_apply = true,
        .reserved0 = 0,
    };

    var env = SimEnv{};
    const ops = env.ops();
    var rpt: rec.Report = undefined;

    // 1. Cut during nv_load fails safely without mutating report.
    rpt.bundle_version = 0xCAFE;
    env.fault = .nv_load_hw_fail;
    try std.testing.expectEqual(Err.hw_error.raw(), rec.boot(&b.hdr, &b.descs, &p, &ops, &rpt));
    try std.testing.expectEqual(@as(u32, 0xCAFE), rpt.bundle_version);

    // 2. Cut during C6 query consumes bounded retry, leaves C6 un-flashed.
    env.fault = .query_fail_c6;
    try std.testing.expectEqual(Err.ok.raw(), rec.boot(&b.hdr, &b.descs, &p, &ops, &rpt));
    // Reprobe recovered on apply in same boot -> both C6 and Display converged cleanly.
    try std.testing.expectEqual(@backingInt(rec.OverallState.converged), rpt.overall_state);
    try std.testing.expectEqual(@as(u32, 1), env.c6_applies);
    try std.testing.expectEqual(@as(u32, 1), env.disp_applies);

    // 3. Dedicated step-by-step power-cut gauntlet with async reboot semantics (`reprobe_after_apply = false`).
    // Every injected power cut must fire cleanly (`fault == .none` after boot) and converge.
    const async_p = rec.Policy{
        .base_backoff_ms = 25,
        .max_backoff_ms = 200,
        .max_retries = 8,
        .backoff_shift = 3,
        .reprobe_after_apply = false,
        .reserved0 = 0,
    };
    const gauntlet = [_]FaultStage{
        .pre_apply_nv_save_cut,
        .apply_cut_before_flash_c6,
        .backoff_delay_cut,
        .apply_flashed_then_cut_c6,
        .apply_cut_before_flash_disp,
        .nv_load_corrupt,
        .final_nv_save_cut,
    };
    var g_env = SimEnv{};
    const g_ops = g_env.ops();

    for (gauntlet) |cut| {
        g_env.fault = cut;
        const rc = rec.boot(&b.hdr, &b.descs, &async_p, &g_ops, &rpt);
        try std.testing.expectEqual(FaultStage.none, g_env.fault);
        if (cut == .pre_apply_nv_save_cut or cut == .backoff_delay_cut or cut == .final_nv_save_cut) {
            try std.testing.expectEqual(Err.hw_error.raw(), rc);
        } else {
            try std.testing.expectEqual(Err.ok.raw(), rc);
        }
    }

    // One clean recovery boot finalizes `.converged` without re-applying C6 (already v20).
    const c6_before_final = g_env.c6_applies;
    try std.testing.expectEqual(Err.ok.raw(), rec.boot(&b.hdr, &b.descs, &async_p, &g_ops, &rpt));
    try std.testing.expectEqual(@backingInt(rec.OverallState.converged), rpt.overall_state);
    try std.testing.expectEqual(@as(u32, 20), g_env.c6_ver);
    try std.testing.expectEqual(@as(u32, 30), g_env.disp_ver);
    try std.testing.expectEqual(c6_before_final, g_env.c6_applies);

    // Steady-state reboot after convergence makes zero applies and zero NV flash writes.
    const saves_before = g_env.nv_save_calls;
    const c6_before = g_env.c6_applies;
    const disp_before = g_env.disp_applies;
    try std.testing.expectEqual(Err.ok.raw(), rec.boot(&b.hdr, &b.descs, &async_p, &g_ops, &rpt));
    try std.testing.expectEqual(@backingInt(rec.OverallState.converged), rpt.overall_state);
    try std.testing.expectEqual(saves_before, g_env.nv_save_calls);
    try std.testing.expectEqual(c6_before, g_env.c6_applies);
    try std.testing.expectEqual(disp_before, g_env.disp_applies);
}

test "apply succeeded on C6 before power cut skips C6 re-apply on next boot" {
    const b = makeBundle(12, 25, 35, 4);
    const p = rec.Policy{
        .base_backoff_ms = 10,
        .max_backoff_ms = 100,
        .max_retries = 3,
        .backoff_shift = 2,
        .reprobe_after_apply = true,
        .reserved0 = 0,
    };

    var env = SimEnv{ .fault = .apply_flashed_then_cut_c6 };
    const ops = env.ops();
    var rpt: rec.Report = undefined;

    // C6 finishes flashing v25, then power drops before Display applies.
    try std.testing.expectEqual(Err.ok.raw(), rec.boot(&b.hdr, &b.descs, &p, &ops, &rpt));
    try std.testing.expectEqual(@as(u32, 1), env.c6_applies);
    try std.testing.expectEqual(@as(u32, 0), env.disp_applies);
    try std.testing.expectEqual(@backingInt(rec.OverallState.pending_retry), rpt.overall_state);

    // Next boot observes C6 == 25 idempotently (no C6 apply!), then applies Display.
    try std.testing.expectEqual(Err.ok.raw(), rec.boot(&b.hdr, &b.descs, &p, &ops, &rpt));
    try std.testing.expectEqual(@as(u32, 1), env.c6_applies);
    try std.testing.expectEqual(@as(u32, 1), env.disp_applies);
    try std.testing.expectEqual(@backingInt(rec.OverallState.converged), rpt.overall_state);
}

test "exhausted retries latch safe_halted with exponential backoff and zero further applies" {
    const b = makeBundle(15, 99, 30, 4);
    const p = rec.Policy{
        .base_backoff_ms = 50,
        .max_backoff_ms = 150,
        .max_retries = 3,
        .backoff_shift = 2,
        .reprobe_after_apply = false,
        .reserved0 = 0,
    };

    var env = SimEnv{ .c6_never_converge = true };
    const ops = env.ops();
    var rpt: rec.Report = undefined;

    // Boot 1..3: 3 applies (retries_used reaches 3; backoffs on retries 2 & 3 = 50, 100).
    for (0..3) |_| {
        try std.testing.expectEqual(Err.ok.raw(), rec.boot(&b.hdr, &b.descs, &p, &ops, &rpt));
        try std.testing.expectEqual(@backingInt(rec.OverallState.pending_retry), rpt.overall_state);
    }
    // Boot 4 checks C6, still stale v10, retries_used == 3 -> enters safe_halted without a 4th apply!
    try std.testing.expectEqual(Err.ok.raw(), rec.boot(&b.hdr, &b.descs, &p, &ops, &rpt));
    try std.testing.expectEqual(@backingInt(rec.OverallState.safe_halted), rpt.overall_state);
    try std.testing.expectEqual(@as(u8, 1), rpt.halted_target_count);
    try std.testing.expectEqual(@as(u32, 3), env.c6_applies);
    try std.testing.expectEqual(@as(u32, 1), env.safe_calls);
    try std.testing.expectEqual(@backingInt(bundle.Target.esp32_c6), env.last_safe_target);
    try std.testing.expectEqualSlices(u32, &[_]u32{ 50, 100 }, env.backoff_log[0..env.backoff_calls]);

    // Subsequent boot stays halted with zero applies and zero NV writes.
    const saves_at_halt = env.nv_save_calls;
    try std.testing.expectEqual(Err.ok.raw(), rec.boot(&b.hdr, &b.descs, &p, &ops, &rpt));
    try std.testing.expectEqual(@backingInt(rec.OverallState.safe_halted), rpt.overall_state);
    try std.testing.expectEqual(@as(u32, 3), env.c6_applies);
    try std.testing.expectEqual(saves_at_halt, env.nv_save_calls);
}

test "new committed bundle_version resets latched safe_halted budget and converges" {
    const b_bad = makeBundle(20, 99, 30, 4);
    const p = rec.Policy{
        .base_backoff_ms = 20,
        .max_backoff_ms = 80,
        .max_retries = 2,
        .backoff_shift = 2,
        .reprobe_after_apply = true,
        .reserved0 = 0,
    };

    var env = SimEnv{ .apply_always_fail_c6 = true };
    const ops = env.ops();
    var rpt: rec.Report = undefined;

    try std.testing.expectEqual(Err.ok.raw(), rec.boot(&b_bad.hdr, &b_bad.descs, &p, &ops, &rpt));
    try std.testing.expectEqual(Err.ok.raw(), rec.boot(&b_bad.hdr, &b_bad.descs, &p, &ops, &rpt));
    try std.testing.expectEqual(@backingInt(rec.OverallState.safe_halted), rpt.overall_state);
    try std.testing.expectEqual(@as(u32, 2), env.c6_applies);

    // Ship recovery bundle_version 21 with working apply -> resets budget cleanly.
    env.apply_always_fail_c6 = false;
    const b_ok = makeBundle(21, 42, 43, 5);
    try std.testing.expectEqual(Err.ok.raw(), rec.boot(&b_ok.hdr, &b_ok.descs, &p, &ops, &rpt));
    try std.testing.expect(rpt.nv_reset_applied);
    try std.testing.expectEqual(@backingInt(rec.OverallState.converged), rpt.overall_state);
    try std.testing.expectEqual(@as(u32, 42), env.c6_ver);
    try std.testing.expectEqual(@as(u32, 43), env.disp_ver);
}

test "compat_proto_ver mismatch forces re-apply and RA8-only bundle converges cleanly" {
    const b = makeBundle(30, 10, 15, 9);
    const p = rec.Policy{
        .base_backoff_ms = 0,
        .max_backoff_ms = 0,
        .max_retries = 2,
        .backoff_shift = 0,
        .reprobe_after_apply = true,
        .reserved0 = 0,
    };
    var env = SimEnv{ .c6_ver = 10, .c6_proto = 4 };
    const ops = env.ops();
    var rpt: rec.Report = undefined;

    // Same image_version (10) but mismatched proto (4 != 9) must reconcile C6.
    try std.testing.expectEqual(Err.ok.raw(), rec.boot(&b.hdr, &b.descs, &p, &ops, &rpt));
    try std.testing.expectEqual(@as(u32, 1), env.c6_applies);
    try std.testing.expectEqual(@as(u32, 9), env.c6_proto);
    try std.testing.expectEqual(@backingInt(rec.OverallState.converged), rpt.overall_state);

    // Single-image RA8-only bundle converges with zero companion queries/applies.
    var ra8_env = SimEnv{};
    const ra8_ops = ra8_env.ops();
    var ra8_hdr = b.hdr;
    ra8_hdr.image_count = 1;
    ra8_hdr.bundle_version = 31;
    try std.testing.expectEqual(Err.ok.raw(), rec.boot(&ra8_hdr, &b.descs, &p, &ra8_ops, &rpt));
    try std.testing.expectEqual(@as(u8, 0), rpt.companions_total);
    try std.testing.expectEqual(@backingInt(rec.OverallState.converged), rpt.overall_state);
    try std.testing.expectEqual(@as(u32, 0), ra8_env.c6_applies);
}

test "reconcile guards null hooks, bad headers, and duplicate targets without mutating report" {
    const b = makeBundle(1, 2, 3, 1);
    const p = rec.Policy{
        .base_backoff_ms = 10,
        .max_backoff_ms = 100,
        .max_retries = 3,
        .backoff_shift = 2,
        .reprobe_after_apply = true,
        .reserved0 = 0,
    };
    var env = SimEnv{};
    var ops = env.ops();
    var rpt: rec.Report = std.mem.zeroes(rec.Report);
    rpt.bundle_version = 0xBEEF_CAFE;

    ops.nv_save = null;
    try std.testing.expectEqual(Err.null_ptr.raw(), rec.boot(&b.hdr, &b.descs, &p, &ops, &rpt));
    try std.testing.expectEqual(@as(u32, 0xBEEF_CAFE), rpt.bundle_version);

    ops = env.ops();
    var bad_hdr = b.hdr;
    bad_hdr.magic = 0;
    try std.testing.expectEqual(Err.invalid_arg.raw(), rec.boot(&bad_hdr, &b.descs, &p, &ops, &rpt));
    try std.testing.expectEqual(@as(u32, 0xBEEF_CAFE), rpt.bundle_version);

    var dup_descs = b.descs;
    dup_descs[2].target = dup_descs[1].target;
    try std.testing.expectEqual(Err.invalid_arg.raw(), rec.boot(&b.hdr, &dup_descs, &p, &ops, &rpt));
    try std.testing.expectEqual(@as(u32, 0xBEEF_CAFE), rpt.bundle_version);
}
