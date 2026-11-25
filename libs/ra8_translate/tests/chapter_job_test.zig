//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! End-to-end Zig tests for `chapter_job`, `chapter_checkpoint`, and C ABI
//! `chapter_job_abi`: golden chapter TRN1 + real `ra8_gfx` overlay fitting,
//! simulated mid-chapter reset/resume byte equivalence, priority/coalescing,
//! yield-to-reader/display, charge vs low-battery pausing, and cancel cleanup.

const std = @import("std");
const err_mod = @import("err");
const job = @import("chapter_job");
const job_abi = @import("chapter_job_abi");
const ckpt = @import("chapter_checkpoint");
const glossary = @import("glossary");
const overlay = @import("overlay");
const sidecar = @import("sidecar");
const gfx = @import("ra8_gfx");
const panel_paint = @import("panel_paint");

const MockCtx = struct {
    ocr_calls: [8]u32 = @splat(0),
    term_calls: u32 = 0,
    batch_calls: u32 = 0,
    batch_items_max: u16 = 0,
    ckpt_len: usize = 0,
    ckpt_cleared: bool = false,
    ckpt_buf: [65536]u8 = undefined,
    trn1_len: usize = 0,
    trn1_buf: [65536]u8 = undefined,
    tgl1_len: usize = 0,
    tgl1_buf: [4096]u8 = undefined,
    fail_on_ocr_page: ?u16 = null,
    load_existing_glossary: bool = false,
};

const ja_tanaka_san = "\xe3\x82\xbf\xe3\x83\x8a\xe3\x82\xab\xe3\x81\x95\xe3\x82\x93"; // Tanaka-san in kana.
const ja_tanaka_stem = "\xe3\x82\xbf\xe3\x83\x8a\xe3\x82\xab"; // Tanaka in katakana.
const ja_wait = "\xe5\xbe\x85\xe3\x81\xa3\xe3\x81\xa6!";
const ja_run = "\xe8\xb5\xb0\xe3\x82\x8d\xe3\x81\x86!";

fn writeSrc(buf: []u8, cursor: *usize, text: []const u8) u32 {
    const off: u32 = @intCast(cursor.*);
    @memcpy(buf[off..][0..text.len], text);
    cursor.* += text.len;
    return off;
}

fn mockOcrPage(
    user: ?*anyopaque,
    chapter: *const job.ChapterSpec,
    page_index: u16,
    max_bubbles_in: u16,
    out_w: *u16,
    out_h: *u16,
    out_bubbles: [*]job.OcrBubble,
    out_count: *u16,
    out_text: [*]u8,
    text_cap: usize,
    out_text_len: *usize,
) callconv(.c) u16 {
    _ = chapter;
    _ = max_bubbles_in;
    const ctx: *MockCtx = @ptrCast(@alignCast(user.?));
    if (ctx.fail_on_ocr_page) |bad_p| {
        if (page_index == bad_p) return err_mod.k_ra8_err_invalid_state;
    }
    if (page_index < ctx.ocr_calls.len) ctx.ocr_calls[page_index] += 1;

    out_w.* = 320;
    out_h.* = 240;
    var cur: usize = 0;
    const text_dst = out_text[0..text_cap];

    switch (page_index) {
        0 => {
            const s0 = ja_tanaka_san ++ " " ++ ja_wait;
            const s1 = ja_run;
            const off0 = writeSrc(text_dst, &cur, s0);
            const off1 = writeSrc(text_dst, &cur, s1);
            out_bubbles[0] = .{
                .x = 20,
                .y = 20,
                .w = 180,
                .h = 90,
                .shape = @backingInt(sidecar.Shape.rounded_rect),
                .text_orientation = @backingInt(sidecar.TextOrientation.horizontal),
                .corner_radius = 10,
                .reading_order = 0,
                .ocr_confidence_bps = 9400,
                .source_off = off0,
                .source_len = @intCast(s0.len),
            };
            out_bubbles[1] = .{
                .x = 220,
                .y = 20,
                .w = 60,
                .h = 180,
                .shape = @backingInt(sidecar.Shape.ellipse),
                .text_orientation = @backingInt(sidecar.TextOrientation.vertical_rl),
                .corner_radius = 0,
                .reading_order = 1,
                .ocr_confidence_bps = 8950,
                .source_off = off1,
                .source_len = @intCast(s1.len),
            };
            out_count.* = 2;
        },
        1 => {
            // Pure artwork spread with zero speech bubbles.
            out_count.* = 0;
        },
        else => {
            const s2 = ja_tanaka_san;
            const off2 = writeSrc(text_dst, &cur, s2);
            out_bubbles[0] = .{
                .x = 40,
                .y = 30,
                .w = 200,
                .h = 100,
                .shape = @backingInt(sidecar.Shape.cloud),
                .text_orientation = @backingInt(sidecar.TextOrientation.horizontal),
                .corner_radius = 0,
                .reading_order = 0,
                .ocr_confidence_bps = 9750,
                .source_off = off2,
                .source_len = @intCast(s2.len),
            };
            out_count.* = 1;
        },
    }
    out_text_len.* = cur;
    return err_mod.k_ra8_ok;
}

fn mockTranslateTerm(
    user: ?*anyopaque,
    source_lang: u8,
    src_utf8: [*]const u8,
    src_len: usize,
    out_eng: [*]u8,
    out_eng_cap: usize,
    out_eng_len: *usize,
) callconv(.c) u16 {
    _ = source_lang;
    const ctx: *MockCtx = @ptrCast(@alignCast(user.?));
    ctx.term_calls += 1;
    const src = src_utf8[0..src_len];
    const eng = if (std.mem.eql(u8, src, ja_tanaka_stem)) "Tanaka" else "Ally";
    if (eng.len > out_eng_cap) return err_mod.k_ra8_err_invalid_size;
    @memcpy(out_eng[0..eng.len], eng);
    out_eng_len.* = eng.len;
    return err_mod.k_ra8_ok;
}

fn mockTranslateBatch(
    user: ?*anyopaque,
    nmt_model: u8,
    src_buf: [*]const u8,
    src_bytes: usize,
    src_spans: [*]const job.Span,
    item_count: u16,
    out_eng_buf: [*]u8,
    out_eng_cap: usize,
    out_eng_spans: [*]job.Span,
) callconv(.c) u16 {
    _ = nmt_model;
    const ctx: *MockCtx = @ptrCast(@alignCast(user.?));
    ctx.batch_calls += 1;
    ctx.batch_items_max = @max(ctx.batch_items_max, item_count);

    var cursor: usize = 0;
    const src = src_buf[0..src_bytes];
    const dst = out_eng_buf[0..out_eng_cap];
    var i: usize = 0;
    while (i < item_count) : (i += 1) {
        const sp = src_spans[i];
        const s = src[sp.off..][0..sp.len];
        var tmp: [256]u8 = undefined;
        const translated = if (std.mem.startsWith(u8, s, "__G00__")) blk: {
            if (std.mem.indexOf(u8, s, ja_wait) != null) {
                break :blk std.fmt.bufPrint(&tmp, "__G00__, wait for me!", .{}) catch unreachable;
            }
            break :blk std.fmt.bufPrint(&tmp, "__G00__!", .{}) catch unreachable;
        } else blk: {
            break :blk "Run now!";
        };
        if (cursor + translated.len > dst.len) return err_mod.k_ra8_err_invalid_size;
        @memcpy(dst[cursor..][0..translated.len], translated);
        out_eng_spans[i] = .{ .off = @intCast(cursor), .len = @intCast(translated.len) };
        cursor += translated.len;
    }
    return err_mod.k_ra8_ok;
}

fn mockSaveCheckpoint(
    user: ?*anyopaque,
    ckpt_bytes: [*]const u8,
    ckpt_len: usize,
) callconv(.c) u16 {
    const ctx: *MockCtx = @ptrCast(@alignCast(user.?));
    if (ckpt_len == 0) {
        ctx.ckpt_len = 0;
        ctx.ckpt_cleared = true;
        return err_mod.k_ra8_ok;
    }
    if (ckpt_len > ctx.ckpt_buf.len) return err_mod.k_ra8_err_invalid_size;
    @memcpy(ctx.ckpt_buf[0..ckpt_len], ckpt_bytes[0..ckpt_len]);
    ctx.ckpt_len = ckpt_len;
    ctx.ckpt_cleared = false;
    return err_mod.k_ra8_ok;
}

fn mockCommitChapter(
    user: ?*anyopaque,
    chapter: *const job.ChapterSpec,
    trn1_bytes: [*]const u8,
    trn1_len: usize,
    tgl1_bytes: [*]const u8,
    tgl1_len: usize,
) callconv(.c) u16 {
    _ = chapter;
    const ctx: *MockCtx = @ptrCast(@alignCast(user.?));
    if (trn1_len > ctx.trn1_buf.len or tgl1_len > ctx.tgl1_buf.len) return err_mod.k_ra8_err_invalid_size;
    @memcpy(ctx.trn1_buf[0..trn1_len], trn1_bytes[0..trn1_len]);
    ctx.trn1_len = trn1_len;
    if (tgl1_len > 0) @memcpy(ctx.tgl1_buf[0..tgl1_len], tgl1_bytes[0..tgl1_len]);
    ctx.tgl1_len = tgl1_len;
    return err_mod.k_ra8_ok;
}

fn testChapterSpec(title_id: u32, order: u32, unread: u8, pages: u16) job.ChapterSpec {
    return .{
        .sidecar_key = .{
            .chapter_hash = @splat(@intCast(order + 1)),
            .detector_model_version = 1,
            .ocr_model_version = 2,
            .nmt_model_version = 3,
            .glossary_revision = 1,
            .source_lang = @backingInt(sidecar.Language.unknown),
            .reserved = .{ 0, 0, 0 },
        },
        .title_id = title_id,
        .chapter_order = order,
        .page_count = pages,
        .lang_hint = @backingInt(sidecar.Language.unknown),
        .unread = unread,
        .trigger = @backingInt(job.Trigger.mdl_download),
        .reserved0 = .{ 0, 0, 0 },
    };
}

fn runSurfaceOverlayPaint(surf: *gfx.Surface) overlay.Paint {
    return panel_paint.table(gfx.Surface, overlay.Paint, surf, gfx.Capabilities.desktop_argb8888);
}

test "golden chapter end-to-end translates, builds glossary, fits over ra8_gfx, and renders TRN1" {
    const work_bytes = job.requiredWorkBytes();
    const work = try std.testing.allocator.alloc(u8, work_bytes);
    defer std.testing.allocator.free(work);

    var surf = try gfx.Surface.init(std.testing.allocator, 320, 240, 0x405060);
    defer surf.deinit(std.testing.allocator);
    const paint = runSurfaceOverlayPaint(&surf);

    var ctx = MockCtx{};
    const hooks = job.Hooks{
        .user = &ctx,
        .paint = &paint,
        .ocr_page = mockOcrPage,
        .translate_term = mockTranslateTerm,
        .translate_batch = mockTranslateBatch,
        .save_checkpoint = mockSaveCheckpoint,
        .commit_chapter = mockCommitChapter,
    };

    const policy = job.Policy{};
    try job.init(work, &policy);
    const ch = testChapterSpec(42, 1, 1, 3);
    try job.enqueue(work, &ch);

    const env = job.EnvState{ .battery_pct = 80, .charging = 0, .reader_active = 0, .display_busy = 0 };
    var st: job.Status = .{};
    var steps: usize = 0;
    while (steps < 16) : (steps += 1) {
        try std.testing.expectEqual(err_mod.k_ra8_ok, job.step(work, &env, &hooks, &st));
        if (st.state == @backingInt(job.State.completed)) break;
    }
    try std.testing.expectEqual(@backingInt(job.State.completed), st.state);
    try std.testing.expect(ctx.ckpt_cleared);
    try std.testing.expectEqual(@as(u16, 2), ctx.batch_items_max);
    try std.testing.expectEqual(@as(u32, 1), ctx.term_calls);

    const view = try sidecar.SidecarView.open(ctx.trn1_buf[0..ctx.trn1_len]);
    try std.testing.expectEqual(@as(u16, 3), view.header.page_count);
    try std.testing.expectEqual(@as(u32, 3), view.header.bubble_count);
    try std.testing.expectEqual(@backingInt(sidecar.Language.japanese), view.header.key.source_lang);
    try std.testing.expectEqualStrings("Tanaka-san, wait for me!", try view.englishText(0));
    try std.testing.expectEqualStrings("Run now!", try view.englishText(1));
    try std.testing.expectEqualStrings("Tanaka-san!", try view.englishText(2));

    var dmg: [8]overlay.Rect = undefined;
    const dmg_n = try overlay.renderPage(&paint, ctx.trn1_buf[0..ctx.trn1_len], 0, policy.overlay_style, &dmg);
    try std.testing.expectEqual(@as(usize, 2), dmg_n);
    try std.testing.expectEqual(@as(u32, 0xFFFFFFFF), surf.pixels[35 * 320 + 60]);
    try std.testing.expectEqual(@as(u32, 0xFF405060), surf.pixels[2 * 320 + 2]);
}

test "mid-chapter reset-resume across OCR, glossary, and translate stages matches golden TRN1 byte-for-byte" {
    const work_bytes = job.requiredWorkBytes();
    const work = try std.testing.allocator.alloc(u8, work_bytes);
    defer std.testing.allocator.free(work);
    const work2 = try std.testing.allocator.alloc(u8, work_bytes);
    defer std.testing.allocator.free(work2);

    var surf = try gfx.Surface.init(std.testing.allocator, 320, 240, 0x405060);
    defer surf.deinit(std.testing.allocator);
    const paint = runSurfaceOverlayPaint(&surf);

    var golden_ctx = MockCtx{};
    const golden_hooks = job.Hooks{
        .user = &golden_ctx,
        .paint = &paint,
        .ocr_page = mockOcrPage,
        .translate_term = mockTranslateTerm,
        .translate_batch = mockTranslateBatch,
        .save_checkpoint = mockSaveCheckpoint,
        .commit_chapter = mockCommitChapter,
    };

    const policy = job.Policy{};
    const env = job.EnvState{ .battery_pct = 90 };
    const ch = testChapterSpec(99, 7, 1, 3);

    var saved_ckpts: [3][8192]u8 = undefined;
    var saved_lens: [3]usize = @splat(0);
    const target_stages = [_]struct { ocr: u16, tr: u16, st: u8 }{
        .{ .ocr = 1, .tr = 0, .st = @backingInt(job.Stage.detect_ocr) },
        .{ .ocr = 3, .tr = 0, .st = @backingInt(job.Stage.translate_fit) },
        .{ .ocr = 3, .tr = 1, .st = @backingInt(job.Stage.translate_fit) },
    };

    try job.init(work, &policy);
    try job.enqueue(work, &ch);
    var st: job.Status = .{};
    while (true) {
        try std.testing.expectEqual(err_mod.k_ra8_ok, job.step(work, &env, &golden_hooks, &st));
        for (target_stages, 0..) |ts, idx| {
            if (st.pages_ocr_done == ts.ocr and st.pages_translated_done == ts.tr and st.stage == ts.st and saved_lens[idx] == 0) {
                @memcpy(saved_ckpts[idx][0..golden_ctx.ckpt_len], golden_ctx.ckpt_buf[0..golden_ctx.ckpt_len]);
                saved_lens[idx] = golden_ctx.ckpt_len;
            }
        }
        if (st.state == @backingInt(job.State.completed)) break;
    }

    // Tampered checkpoint fails closed with CrcMismatch before altering active state.
    saved_ckpts[0][saved_lens[0] - 1] ^= 0x55;
    try std.testing.expectError(job.Error.CrcMismatch, job.restoreCheckpoint(work, saved_ckpts[0][0..saved_lens[0]]));
    saved_ckpts[0][saved_lens[0] - 1] ^= 0x55;

    for (target_stages, 0..) |ts, idx| {
        @memset(work2, 0xAA);
        try job.init(work2, &policy);
        try job.restoreCheckpoint(work2, saved_ckpts[idx][0..saved_lens[idx]]);

        var res_ctx = MockCtx{};
        const res_hooks = job.Hooks{
            .user = &res_ctx,
            .paint = &paint,
            .ocr_page = mockOcrPage,
            .translate_term = mockTranslateTerm,
            .translate_batch = mockTranslateBatch,
            .save_checkpoint = mockSaveCheckpoint,
            .commit_chapter = mockCommitChapter,
        };
        var rst: job.Status = .{};
        var guard: usize = 0;
        while (guard < 16) : (guard += 1) {
            try std.testing.expectEqual(err_mod.k_ra8_ok, job.step(work2, &env, &res_hooks, &rst));
            if (rst.state == @backingInt(job.State.completed)) break;
        }
        try std.testing.expectEqual(@backingInt(job.State.completed), rst.state);

        // Already-checkpointed pages are not re-OCRed after reboot.
        var p: u16 = 0;
        while (p < ts.ocr) : (p += 1) {
            try std.testing.expectEqual(@as(u32, 0), res_ctx.ocr_calls[p]);
        }
        if (ts.st == @backingInt(job.Stage.translate_fit)) {
            try std.testing.expectEqual(@as(u32, 0), res_ctx.term_calls);
        }

        try std.testing.expectEqualSlices(u8, golden_ctx.trn1_buf[0..golden_ctx.trn1_len], res_ctx.trn1_buf[0..res_ctx.trn1_len]);
        try std.testing.expectEqualSlices(u8, golden_ctx.tgl1_buf[0..golden_ctx.tgl1_len], res_ctx.tgl1_buf[0..res_ctx.tgl1_len]);
    }
}

test "queue selects unread chapters in order without preempting active chapter, and coalesces" {
    const work_bytes = job.requiredWorkBytes();
    const work = try std.testing.allocator.alloc(u8, work_bytes);
    defer std.testing.allocator.free(work);

    var surf = try gfx.Surface.init(std.testing.allocator, 320, 240, 0xFFFFFF);
    defer surf.deinit(std.testing.allocator);
    const paint = runSurfaceOverlayPaint(&surf);

    var ctx = MockCtx{};
    const hooks = job.Hooks{
        .user = &ctx,
        .paint = &paint,
        .ocr_page = mockOcrPage,
        .translate_term = mockTranslateTerm,
        .translate_batch = mockTranslateBatch,
        .save_checkpoint = mockSaveCheckpoint,
        .commit_chapter = mockCommitChapter,
    };

    try job.init(work, &.{});
    try job.enqueue(work, &testChapterSpec(10, 5, 0, 1));
    var user_ch4 = testChapterSpec(10, 4, 1, 1);
    user_ch4.trigger = @backingInt(job.Trigger.user_request);
    try job.enqueue(work, &user_ch4);
    try job.enqueue(work, &testChapterSpec(10, 2, 1, 2));
    try job.enqueue(work, &testChapterSpec(10, 2, 1, 2)); // Duplicate coalesces.

    const env = job.EnvState{ .battery_pct = 90 };
    var st: job.Status = .{};
    try job.getStatus(work, &st);
    try std.testing.expectEqual(@as(u16, 3), st.queued_count);

    // First step activates unread lowest order (chapter 2).
    try std.testing.expectEqual(err_mod.k_ra8_ok, job.step(work, &env, &hooks, &st));
    try std.testing.expectEqual(@as(u32, 2), st.active_chapter_order);
    try std.testing.expectEqual(job.ChapterState.translating, try job.queryChapter(work, 10, 2));
    try std.testing.expectEqual(job.ChapterState.queued, try job.queryChapter(work, 10, 4));

    // Late arrival of chapter 1 (unread) does not preempt active chapter 2.
    try job.enqueue(work, &testChapterSpec(10, 1, 1, 1));
    try std.testing.expectEqual(job.ChapterState.queued, try job.queryChapter(work, 10, 1));

    var order_log: [4]u32 = undefined;
    var done_n: usize = 0;
    var last_order: u32 = st.active_chapter_order;
    var guard: usize = 0;
    while (guard < 40) : (guard += 1) {
        try std.testing.expectEqual(err_mod.k_ra8_ok, job.step(work, &env, &hooks, &st));
        if (st.state == @backingInt(job.State.completed)) {
            order_log[done_n] = last_order;
            done_n += 1;
            if (done_n == 4) break;
        } else if (st.has_active != 0) {
            last_order = st.active_chapter_order;
        }
    }
    try std.testing.expectEqual(@as(usize, 4), done_n);
    try std.testing.expectEqualSlices(u32, &[_]u32{ 2, 1, 4, 5 }, &order_log);
}

test "yields to reader/display, pauses below battery threshold on battery, and runs freely on charge" {
    const work_bytes = job.requiredWorkBytes();
    const work = try std.testing.allocator.alloc(u8, work_bytes);
    defer std.testing.allocator.free(work);

    var surf = try gfx.Surface.init(std.testing.allocator, 320, 240, 0xFFFFFF);
    defer surf.deinit(std.testing.allocator);
    const paint = runSurfaceOverlayPaint(&surf);

    var ctx = MockCtx{};
    const hooks = job.Hooks{
        .user = &ctx,
        .paint = &paint,
        .ocr_page = mockOcrPage,
        .translate_term = mockTranslateTerm,
        .translate_batch = mockTranslateBatch,
        .save_checkpoint = mockSaveCheckpoint,
        .commit_chapter = mockCommitChapter,
    };

    const policy = job.Policy{
        .battery_min_pct = 25,
        .battery_hysteresis_pct = 5,
    };
    try job.init(work, &policy);
    try job.enqueue(work, &testChapterSpec(3, 1, 1, 3));

    var st: job.Status = .{};
    // Advance page 0 OCR.
    try std.testing.expectEqual(err_mod.k_ra8_ok, job.step(work, &.{ .battery_pct = 50 }, &hooks, &st));
    try std.testing.expectEqual(@as(u16, 1), st.pages_ocr_done);

    // Reader active -> yields without invoking OCR.
    try std.testing.expectEqual(err_mod.k_ra8_ok, job.step(work, &.{ .battery_pct = 50, .reader_active = 1 }, &hooks, &st));
    try std.testing.expectEqual(@backingInt(job.State.yielded), st.state);
    try std.testing.expectEqual(@backingInt(job.PauseReason.reader_active), st.pause_reason);
    try std.testing.expectEqual(@as(u16, 1), st.pages_ocr_done);
    try std.testing.expectEqual(job.ChapterState.paused, try job.queryChapter(work, 3, 1));

    // Display busy -> yields cleanly.
    try std.testing.expectEqual(err_mod.k_ra8_ok, job.step(work, &.{ .battery_pct = 50, .display_busy = 1 }, &hooks, &st));
    try std.testing.expectEqual(@backingInt(job.State.yielded), st.state);
    try std.testing.expectEqual(@backingInt(job.PauseReason.display_busy), st.pause_reason);
    try std.testing.expectEqual(@as(u16, 1), st.pages_ocr_done);

    // Low battery on battery power -> paused_battery, held by hysteresis at 28%.
    try std.testing.expectEqual(err_mod.k_ra8_ok, job.step(work, &.{ .battery_pct = 24, .charging = 0 }, &hooks, &st));
    try std.testing.expectEqual(@backingInt(job.State.paused_battery), st.state);
    try std.testing.expectEqual(@backingInt(job.PauseReason.low_battery), st.pause_reason);
    try std.testing.expectEqual(err_mod.k_ra8_ok, job.step(work, &.{ .battery_pct = 28, .charging = 0 }, &hooks, &st));
    try std.testing.expectEqual(@backingInt(job.State.paused_battery), st.state);
    try std.testing.expectEqual(@as(u16, 1), st.pages_ocr_done);

    // Charging runs freely regardless of low battery percentage.
    try std.testing.expectEqual(err_mod.k_ra8_ok, job.step(work, &.{ .battery_pct = 12, .charging = 1 }, &hooks, &st));
    try std.testing.expectEqual(@backingInt(job.State.running), st.state);
    try std.testing.expectEqual(@as(u16, 2), st.pages_ocr_done);
}

test "C ABI job exports handle unaligned buffer, cancel cleanup, and null guards" {
    var work_bytes: usize = 0;
    try std.testing.expectEqual(err_mod.k_ra8_err_null_ptr, job_abi.ra8_translate_job_work_bytes(null));
    try std.testing.expectEqual(err_mod.k_ra8_ok, job_abi.ra8_translate_job_work_bytes(&work_bytes));

    // Allocate + 1 byte to verify unaligned caller slices never trap or fail.
    const raw = try std.testing.allocator.alloc(u8, work_bytes + 1);
    defer std.testing.allocator.free(raw);
    const work = raw[1..];

    var surf = try gfx.Surface.init(std.testing.allocator, 320, 240, 0xFFFFFF);
    defer surf.deinit(std.testing.allocator);
    const paint = runSurfaceOverlayPaint(&surf);

    var ctx = MockCtx{};
    const hooks = job_abi.JobHooks{
        .user = &ctx,
        .paint = &paint,
        .ocr_page = mockOcrPage,
        .translate_batch = mockTranslateBatch,
        .save_checkpoint = mockSaveCheckpoint,
        .commit_chapter = mockCommitChapter,
    };

    const pol = job_abi.JobPolicy{};
    try std.testing.expectEqual(err_mod.k_ra8_err_null_ptr, job_abi.ra8_translate_job_init(null, work.len, &pol));
    try std.testing.expectEqual(err_mod.k_ra8_ok, job_abi.ra8_translate_job_init(work.ptr, work.len, &pol));

    const ch1 = testChapterSpec(77, 1, 1, 2);
    const ch2 = testChapterSpec(77, 2, 1, 1);
    try std.testing.expectEqual(err_mod.k_ra8_ok, job_abi.ra8_translate_job_enqueue(work.ptr, work.len, &ch1));
    try std.testing.expectEqual(err_mod.k_ra8_ok, job_abi.ra8_translate_job_enqueue(work.ptr, work.len, &ch2));

    var st: job_abi.JobStatus = .{};
    const env = job_abi.JobEnv{ .battery_pct = 90 };
    try std.testing.expectEqual(err_mod.k_ra8_ok, job_abi.ra8_translate_job_step(work.ptr, work.len, &env, &hooks, &st));
    try std.testing.expect(ctx.ckpt_len > 0);

    var qstate: u8 = 0;
    try std.testing.expectEqual(err_mod.k_ra8_ok, job_abi.ra8_translate_job_query_chapter(work.ptr, work.len, 77, 1, &qstate));
    try std.testing.expectEqual(@backingInt(job.ChapterState.translating), qstate);

    // Cancel clears active NV checkpoint cleanly and drops active chapter.
    try std.testing.expectEqual(err_mod.k_ra8_ok, job_abi.ra8_translate_job_cancel(work.ptr, work.len, 77, 1, &hooks));
    try std.testing.expect(ctx.ckpt_cleared);
    try std.testing.expectEqual(err_mod.k_ra8_ok, job_abi.ra8_translate_job_status(work.ptr, work.len, &st));
    try std.testing.expectEqual(@as(u8, 0), st.has_active);
    try std.testing.expectEqual(@as(u16, 1), st.queued_count);
}
