//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for `inc/ra8_translate_job.h`: exposes the freestanding
//! resumable background chapter translation scheduler over caller workspaces.

const std = @import("std");
const err_mod = @import("err");
const job = @import("chapter_job");
const sidecar = @import("sidecar");
const overlay = @import("overlay");

pub const JobChapter = job.ChapterSpec;
pub const JobPolicy = job.Policy;
pub const JobEnv = job.EnvState;
pub const JobOcrBubble = job.OcrBubble;
pub const JobSpan = job.Span;
pub const JobStatus = job.Status;
pub const JobHooks = job.Hooks;

comptime {
    const ptr_bytes = @sizeOf(usize);
    std.debug.assert(@sizeOf(JobChapter) == 68 and @alignOf(JobChapter) == 4);
    std.debug.assert(@offsetOf(JobChapter, "sidecar_key") == 0);
    std.debug.assert(@offsetOf(JobChapter, "title_id") == 52);
    std.debug.assert(@offsetOf(JobChapter, "chapter_order") == 56);
    std.debug.assert(@offsetOf(JobChapter, "page_count") == 60);
    std.debug.assert(@offsetOf(JobChapter, "lang_hint") == 62);
    std.debug.assert(@offsetOf(JobChapter, "unread") == 63);
    std.debug.assert(@offsetOf(JobChapter, "trigger") == 64);

    std.debug.assert(@sizeOf(JobPolicy) == 32 and @alignOf(JobPolicy) == 4);
    std.debug.assert(@offsetOf(JobPolicy, "overlay_style") == 8);
    std.debug.assert(@sizeOf(JobEnv) == 4 and @alignOf(JobEnv) == 1);
    std.debug.assert(@sizeOf(JobOcrBubble) == 24 and @alignOf(JobOcrBubble) == 4);
    std.debug.assert(@sizeOf(JobSpan) == 8 and @alignOf(JobSpan) == 4);
    std.debug.assert(@sizeOf(JobStatus) == 32 and @alignOf(JobStatus) == 4);
    std.debug.assert(@sizeOf(JobHooks) == ptr_bytes * 8 and @alignOf(JobHooks) == @alignOf(?*anyopaque));

    std.debug.assert(@sizeOf(sidecar.SidecarKey) == 52);
    std.debug.assert(@sizeOf(overlay.Style) == 24);
}

fn inputSlice(comptime T: type, ptr_opt: ?[*]const T, count: usize) ?[]const T {
    if (ptr_opt) |p| return p[0..count];
    if (count == 0) return &.{};
    return null;
}

/// C ABI export for `ra8_translate_job_work_bytes()`.
pub export fn ra8_translate_job_work_bytes(out_bytes_opt: ?*usize) callconv(.c) u16 {
    const out_bytes = out_bytes_opt orelse return err_mod.k_ra8_err_null_ptr;
    out_bytes.* = job.requiredWorkBytes();
    return err_mod.k_ra8_ok;
}

/// C ABI export for `ra8_translate_job_init()`.
pub export fn ra8_translate_job_init(
    work_opt: ?[*]u8,
    work_cap: usize,
    policy_opt: ?*const JobPolicy,
) callconv(.c) u16 {
    const work_buf = work_opt orelse return err_mod.k_ra8_err_null_ptr;
    const policy = policy_opt orelse return err_mod.k_ra8_err_null_ptr;
    job.init(work_buf[0..work_cap], policy) catch |e| return err_mod.toRa8Err(e);
    return err_mod.k_ra8_ok;
}

/// C ABI export for `ra8_translate_job_enqueue()`.
pub export fn ra8_translate_job_enqueue(
    work_opt: ?[*]u8,
    work_cap: usize,
    chapter_opt: ?*const JobChapter,
) callconv(.c) u16 {
    const work_buf = work_opt orelse return err_mod.k_ra8_err_null_ptr;
    const chapter = chapter_opt orelse return err_mod.k_ra8_err_null_ptr;
    job.enqueue(work_buf[0..work_cap], chapter) catch |e| return err_mod.toRa8Err(e);
    return err_mod.k_ra8_ok;
}

/// C ABI export for `ra8_translate_job_cancel()`.
pub export fn ra8_translate_job_cancel(
    work_opt: ?[*]u8,
    work_cap: usize,
    title_id: u32,
    chapter_order: u32,
    hooks_opt: ?*const JobHooks,
) callconv(.c) u16 {
    const work_buf = work_opt orelse return err_mod.k_ra8_err_null_ptr;
    job.cancel(work_buf[0..work_cap], title_id, chapter_order, hooks_opt) catch |e| {
        return err_mod.toRa8Err(e);
    };
    return err_mod.k_ra8_ok;
}

/// C ABI export for `ra8_translate_job_restore_checkpoint()`.
pub export fn ra8_translate_job_restore_checkpoint(
    work_opt: ?[*]u8,
    work_cap: usize,
    ckpt_opt: ?[*]const u8,
    ckpt_len: usize,
) callconv(.c) u16 {
    const work_buf = work_opt orelse return err_mod.k_ra8_err_null_ptr;
    const ckpt_bytes = inputSlice(u8, ckpt_opt, ckpt_len) orelse return err_mod.k_ra8_err_null_ptr;
    job.restoreCheckpoint(work_buf[0..work_cap], ckpt_bytes) catch |e| return err_mod.toRa8Err(e);
    return err_mod.k_ra8_ok;
}

/// C ABI export for `ra8_translate_job_step()`.
pub export fn ra8_translate_job_step(
    work_opt: ?[*]u8,
    work_cap: usize,
    env_opt: ?*const JobEnv,
    hooks_opt: ?*const JobHooks,
    out_status_opt: ?*JobStatus,
) callconv(.c) u16 {
    const out_status = out_status_opt orelse return err_mod.k_ra8_err_null_ptr;
    const work_buf = work_opt orelse return err_mod.k_ra8_err_null_ptr;
    const env = env_opt orelse return err_mod.k_ra8_err_null_ptr;
    const hooks = hooks_opt orelse return err_mod.k_ra8_err_null_ptr;
    return job.step(work_buf[0..work_cap], env, hooks, out_status);
}

/// C ABI export for `ra8_translate_job_status()`.
pub export fn ra8_translate_job_status(
    work_opt: ?[*]const u8,
    work_cap: usize,
    out_status_opt: ?*JobStatus,
) callconv(.c) u16 {
    const out_status = out_status_opt orelse return err_mod.k_ra8_err_null_ptr;
    const work_buf = work_opt orelse return err_mod.k_ra8_err_null_ptr;
    job.getStatus(work_buf[0..work_cap], out_status) catch |e| return err_mod.toRa8Err(e);
    return err_mod.k_ra8_ok;
}

/// C ABI export for `ra8_translate_job_query_chapter()`.
pub export fn ra8_translate_job_query_chapter(
    work_opt: ?[*]const u8,
    work_cap: usize,
    title_id: u32,
    chapter_order: u32,
    out_state_opt: ?*u8,
) callconv(.c) u16 {
    const out_state = out_state_opt orelse return err_mod.k_ra8_err_null_ptr;
    const work_buf = work_opt orelse return err_mod.k_ra8_err_null_ptr;
    const st = job.queryChapter(work_buf[0..work_cap], title_id, chapter_order) catch |e| {
        return err_mod.toRa8Err(e);
    };
    out_state.* = @backingInt(st);
    return err_mod.k_ra8_ok;
}
