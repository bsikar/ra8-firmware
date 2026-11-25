//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Freestanding, zero-allocation background chapter translation scheduler
//! tying together NPU detect+OCR, script language selection, per-series
//! glossary extraction/masking/restoration, batched NMT translation, `ra8_widget`
//! overlay typesetting, battery/reader/display yield gating, unread-first queue
//! ordering, and CRC-32 per-page NV checkpoint recovery (`TRJC` -> `TRN1`).

const std = @import("std");
const ckpt = @import("chapter_checkpoint");
const err_mod = @import("err");
const glossary = @import("glossary");
const lang = @import("lang");
const overlay = @import("overlay");
const sidecar = @import("sidecar");

pub const Error = err_mod.TranslateError;

pub const Trigger = ckpt.Trigger;
pub const State = ckpt.State;
pub const Stage = ckpt.Stage;
pub const PauseReason = ckpt.PauseReason;
pub const ChapterState = ckpt.ChapterState;
pub const ChapterSpec = ckpt.ChapterSpec;

pub const max_queue = ckpt.max_queue;
pub const max_pages = ckpt.max_pages;
pub const max_bubbles = ckpt.max_bubbles;
pub const max_page_bubbles = ckpt.max_page_bubbles;
pub const max_line_breaks = ckpt.max_line_breaks;
pub const max_text_pool = ckpt.max_text_pool;
pub const max_batch = ckpt.max_batch;
pub const default_batch = ckpt.default_batch;
pub const max_glossary_bytes = ckpt.max_glossary_bytes;

const workspace_magic: u32 = 0x574A5254; // Little-endian "TRJW".

/// C ABI scheduler policy configuration (`ra8_translate_job_policy_t`).
pub const Policy = extern struct {
    battery_min_pct: u8 = ckpt.default_batt_pct,
    battery_hysteresis_pct: u8 = 0,
    nmt_batch_max: u8 = ckpt.default_batch,
    yield_to_reader: u8 = 1,
    yield_to_display: u8 = 1,
    reserved0: [3]u8 = .{ 0, 0, 0 },
    overlay_style: overlay.Style = .{},
};

/// C ABI power and foreground contention sample (`ra8_translate_job_env_t`).
pub const EnvState = ckpt.EnvState;
/// C ABI OCR page-detector output item (`ra8_translate_job_ocr_bubble_t`).
pub const OcrBubble = ckpt.OcrBubble;
/// C ABI byte span within flat batch translation buffers (`ra8_translate_job_span_t`).
pub const Span = ckpt.Span;
/// C ABI observable scheduler snapshot (`ra8_translate_job_status_t`).
pub const Status = ckpt.Status;

/// C ABI injected pipeline callbacks (`ra8_translate_job_hooks_t`).
pub const Hooks = extern struct {
    user: ?*anyopaque = null,
    paint: ?*const overlay.Paint = null,
    ocr_page: ?*const fn (
        user: ?*anyopaque,
        chapter: *const ChapterSpec,
        page_index: u16,
        max_bubbles_in: u16,
        out_page_w: *u16,
        out_page_h: *u16,
        out_bubbles: [*]OcrBubble,
        out_bubble_count: *u16,
        out_text_buf: [*]u8,
        text_cap: usize,
        out_text_len: *usize,
    ) callconv(.c) u16 = null,
    translate_term: ?*const fn (
        user: ?*anyopaque,
        source_lang: u8,
        src_utf8: [*]const u8,
        src_len: usize,
        out_eng_utf8: [*]u8,
        out_eng_cap: usize,
        out_eng_len: *usize,
    ) callconv(.c) u16 = null,
    translate_batch: ?*const fn (
        user: ?*anyopaque,
        nmt_model: u8,
        src_buf: [*]const u8,
        src_bytes: usize,
        src_spans: [*]const Span,
        item_count: u16,
        out_eng_buf: [*]u8,
        out_eng_cap: usize,
        out_eng_spans: [*]Span,
    ) callconv(.c) u16 = null,
    load_glossary: ?*const fn (
        user: ?*anyopaque,
        title_id: u32,
        out_tgl_buf: [*]u8,
        out_tgl_cap: usize,
        out_tgl_len: *usize,
    ) callconv(.c) u16 = null,
    save_checkpoint: ?*const fn (
        user: ?*anyopaque,
        ckpt_bytes: [*]const u8,
        ckpt_len: usize,
    ) callconv(.c) u16 = null,
    commit_chapter: ?*const fn (
        user: ?*anyopaque,
        chapter: *const ChapterSpec,
        trn1_bytes: [*]const u8,
        trn1_len: usize,
        tgl1_bytes: [*]const u8,
        tgl1_len: usize,
    ) callconv(.c) u16 = null,
};

const QueueItem = struct {
    spec: ChapterSpec = .{},
    seq: u64 = 0,
};

const io_buf_bytes: usize = 36864;
const stage_buf_bytes: usize = 4096;

/// Caller-owned workspace holding all scheduler state and bounded scratch pools.
const Workspace = struct {
    magic: u32 = workspace_magic,
    policy: Policy = .{},
    queue: [max_queue]QueueItem = @splat(QueueItem{}),
    queue_len: u16 = 0,
    next_enqueue_seq: u64 = 1,

    has_active: bool = false,
    battery_paused: bool = false,
    state: State = .idle,
    stage: Stage = .none,
    pause_reason: PauseReason = .none,
    last_error: u16 = err_mod.k_ra8_ok,
    resolved_lang: sidecar.Language = .unknown,

    active: ChapterSpec = .{},
    checkpoint_seq: u32 = 0,
    pages_ocr_done: u16 = 0,
    pages_translated_done: u16 = 0,
    bubble_count: u16 = 0,
    line_break_count: u32 = 0,
    text_pool_bytes: u32 = 0,
    tgl1_len: u32 = 0,

    pages: [max_pages]sidecar.PageEntry = undefined,
    bubbles: [max_bubbles]sidecar.BubbleRecord = undefined,
    line_breaks: [max_line_breaks]u16 = undefined,
    text_pool: [max_text_pool]u8 = undefined,
    tgl1_buf: [max_glossary_bytes]u8 = undefined,
    tgl1_scratch: [max_glossary_bytes]u8 = undefined,
    glossary_tbl: glossary.GlossaryTable = .{},

    io_buf: [io_buf_bytes]u8 = undefined,
    stage_text_a: [stage_buf_bytes]u8 = undefined,
    stage_text_b: [stage_buf_bytes]u8 = undefined,
    stage_restore: [stage_buf_bytes]u8 = undefined,
    stage_ocr_bubbles: [max_page_bubbles]OcrBubble = undefined,
    stage_plans: [max_batch]glossary.SubstitutionPlan = undefined,
    slice_ptrs: [max_bubbles][]const u8 = undefined,
    candidates: [glossary.max_entries]glossary.Candidate = undefined,
};

/// Total byte size caller must supply for `work_buf`, including alignment margin.
pub fn requiredWorkBytes() usize {
    return @sizeOf(Workspace) + (@alignOf(Workspace) - 1);
}

fn bindMut(work_buf: []u8) Error!*Workspace {
    if (work_buf.len < requiredWorkBytes()) return Error.InvalidSize;
    const align_n = @alignOf(Workspace);
    const addr = @intFromPtr(work_buf.ptr);
    const off = (align_n - (addr & (align_n - 1))) & (align_n - 1);
    const ptr: *Workspace = @ptrCast(@alignCast(work_buf.ptr + off));
    return ptr;
}

fn bindInitializedMut(work_buf: []u8) Error!*Workspace {
    const ws = try bindMut(work_buf);
    if (ws.magic != workspace_magic) return Error.InvalidArg;
    return ws;
}

fn bindInitializedConst(work_buf: []const u8) Error!*const Workspace {
    if (work_buf.len < requiredWorkBytes()) return Error.InvalidSize;
    const align_n = @alignOf(Workspace);
    const addr = @intFromPtr(work_buf.ptr);
    const off = (align_n - (addr & (align_n - 1))) & (align_n - 1);
    const ws: *const Workspace = @ptrCast(@alignCast(work_buf.ptr + off));
    if (ws.magic != workspace_magic) return Error.InvalidArg;
    return ws;
}

fn validatePolicy(p: *const Policy) Error!void {
    if (p.battery_min_pct == 0 or p.battery_min_pct > 100) return Error.InvalidArg;
    if (p.battery_hysteresis_pct > 50) return Error.InvalidArg;
    if (p.nmt_batch_max > max_batch) return Error.InvalidArg;
    if (p.yield_to_reader > 1 or p.yield_to_display > 1) return Error.InvalidArg;
    if (!allZero(&p.reserved0)) return Error.InvalidArg;
    try overlay.validateStyle(p.overlay_style);
}

/// Initialize caller-owned workspace with validated scheduler policy.
pub fn init(work_buf: []u8, policy: *const Policy) Error!void {
    try validatePolicy(policy);
    const ws = try bindMut(work_buf);
    ws.magic = workspace_magic;
    ws.policy = policy.*;
    ws.queue_len = 0;
    ws.next_enqueue_seq = 1;
    ws.has_active = false;
    ws.battery_paused = false;
    ws.state = .idle;
    ws.stage = .none;
    ws.pause_reason = .none;
    ws.last_error = err_mod.k_ra8_ok;
    ws.resolved_lang = .unknown;
    ws.active = .{};
    ws.checkpoint_seq = 0;
    ws.pages_ocr_done = 0;
    ws.pages_translated_done = 0;
    ws.bubble_count = 0;
    ws.line_break_count = 0;
    ws.text_pool_bytes = 0;
    ws.tgl1_len = 0;
}

/// Add a chapter request or coalesce cleanly if `(title_id, chapter_order)` matches existing entry.
pub fn enqueue(work_buf: []u8, chapter: *const ChapterSpec) Error!void {
    const ws = try bindInitializedMut(work_buf);
    try ckpt.validateChapterSpec(chapter);

    if (ws.has_active and sameChapter(&ws.active, chapter)) {
        if (chapter.unread != 0) ws.active.unread = 1;
        return;
    }
    var i: usize = 0;
    while (i < ws.queue_len) : (i += 1) {
        if (sameChapter(&ws.queue[i].spec, chapter)) {
            if (chapter.unread != 0) ws.queue[i].spec.unread = 1;
            ws.queue[i].spec.trigger = chapter.trigger;
            ws.queue[i].spec.sidecar_key = chapter.sidecar_key;
            ws.queue[i].spec.page_count = chapter.page_count;
            ws.queue[i].spec.lang_hint = chapter.lang_hint;
            return;
        }
    }
    if (ws.queue_len >= max_queue) return Error.InvalidSize;
    ws.queue[ws.queue_len] = .{
        .spec = chapter.*,
        .seq = ws.next_enqueue_seq,
    };
    ws.queue_len += 1;
    ws.next_enqueue_seq += 1;
}

/// Cancel a queued or active chapter, clearing active NV checkpoint via `hooks` if provided.
pub fn cancel(work_buf: []u8, title_id: u32, chapter_order: u32, hooks: ?*const Hooks) Error!void {
    const ws = try bindInitializedMut(work_buf);
    if (ws.has_active and ws.active.title_id == title_id and ws.active.chapter_order == chapter_order) {
        clearActiveChapter(ws);
        if (hooks) |h| {
            if (h.save_checkpoint) |save_fn| {
                const empty: [1]u8 = .{0};
                _ = save_fn(h.user, &empty, 0);
            }
        }
        return;
    }

    var i: usize = 0;
    while (i < ws.queue_len) : (i += 1) {
        const q = ws.queue[i].spec;
        if (q.title_id == title_id and q.chapter_order == chapter_order) {
            var j: usize = i + 1;
            while (j < ws.queue_len) : (j += 1) {
                ws.queue[j - 1] = ws.queue[j];
            }
            ws.queue_len -= 1;
            return;
        }
    }
    return Error.NotFound;
}

/// Restore interrupted active chapter state from a CRC-32-checked `TRJC` checkpoint blob.
pub fn restoreCheckpoint(work_buf: []u8, ckpt_bytes: []const u8) Error!void {
    const ws = try bindInitializedMut(work_buf);
    var next_chapter: ChapterSpec = undefined;
    var next_stage: Stage = undefined;
    var next_lang: sidecar.Language = undefined;
    var next_seq: u32 = 0;
    var next_ocr: u16 = 0;
    var next_tr: u16 = 0;
    var next_bubbles: u16 = 0;
    var next_breaks: u32 = 0;
    var next_pool: u32 = 0;
    var next_tgl: u32 = 0;

    try ckpt.restoreCheckpoint(ckpt_bytes, .{
        .chapter = &next_chapter,
        .stage = &next_stage,
        .resolved_lang = &next_lang,
        .checkpoint_seq = &next_seq,
        .pages_ocr_done = &next_ocr,
        .pages_translated_done = &next_tr,
        .bubble_count = &next_bubbles,
        .line_break_count = &next_breaks,
        .text_pool_bytes = &next_pool,
        .tgl1_len = &next_tgl,
        .pages = &ws.pages,
        .bubbles = &ws.bubbles,
        .line_breaks = &ws.line_breaks,
        .text_pool = &ws.text_pool,
        .tgl1_bytes = &ws.tgl1_buf,
    });

    if (next_tgl > 0) {
        try ws.glossary_tbl.load(ws.tgl1_buf[0..next_tgl]);
    } else {
        ws.glossary_tbl = glossary.GlossaryTable.init(next_lang, next_chapter.sidecar_key.glossary_revision);
    }

    ws.active = next_chapter;
    ws.has_active = true;
    ws.stage = next_stage;
    ws.resolved_lang = next_lang;
    ws.checkpoint_seq = next_seq;
    ws.pages_ocr_done = next_ocr;
    ws.pages_translated_done = next_tr;
    ws.bubble_count = next_bubbles;
    ws.line_break_count = next_breaks;
    ws.text_pool_bytes = next_pool;
    ws.tgl1_len = next_tgl;
    ws.state = .running;
    ws.pause_reason = .none;
    ws.last_error = err_mod.k_ra8_ok;
    removeMatchingQueueEntry(ws, &ws.active);
}

/// Inspect scheduler snapshot without mutating state.
pub fn getStatus(work_buf: []const u8, out_status: *Status) Error!void {
    const ws = try bindInitializedConst(work_buf);
    out_status.* = buildStatus(ws, ws.state);
}

/// Query queued/active state of `(title_id, chapter_order)` for reader UI badges.
pub fn queryChapter(work_buf: []const u8, title_id: u32, chapter_order: u32) Error!ChapterState {
    const ws = try bindInitializedConst(work_buf);
    if (ws.has_active and ws.active.title_id == title_id and ws.active.chapter_order == chapter_order) {
        if (ws.state == .yielded or ws.state == .paused_battery) return .paused;
        return .translating;
    }
    var i: usize = 0;
    while (i < ws.queue_len) : (i += 1) {
        if (ws.queue[i].spec.title_id == title_id and ws.queue[i].spec.chapter_order == chapter_order) {
            return .queued;
        }
    }
    return .none;
}

/// Advance at most one scheduling decision or per-page pipeline increment.
pub fn step(
    work_buf: []u8,
    env: *const EnvState,
    hooks: *const Hooks,
    out_status: *Status,
) u16 {
    const ws = bindInitializedMut(work_buf) catch |e| return err_mod.toRa8Err(e);
    if (env.battery_pct > 100 or env.charging > 1 or env.reader_active > 1 or env.display_busy > 1) {
        return err_mod.toRa8Err(Error.InvalidArg);
    }

    if (ws.state == .failed and ws.has_active) {
        out_status.* = buildStatus(ws, .failed);
        return ws.last_error;
    }

    if (applyPowerAndYieldGate(ws, env)) {
        out_status.* = buildStatus(ws, ws.state);
        return err_mod.k_ra8_ok;
    }

    if (!ws.has_active) {
        if (ws.queue_len == 0) {
            ws.state = .idle;
            ws.stage = .none;
            ws.pause_reason = .none;
            out_status.* = buildStatus(ws, .idle);
            return err_mod.k_ra8_ok;
        }
        activateNextQueued(ws);
    }

    ws.state = .running;
    ws.pause_reason = .none;
    const rc = advanceActiveStage(ws, hooks);
    if (rc != err_mod.k_ra8_ok) {
        ws.state = .failed;
        ws.last_error = rc;
        out_status.* = buildStatus(ws, .failed);
        return rc;
    }

    if (!ws.has_active and ws.stage == .done) {
        out_status.* = buildCompletedSnapshot(ws);
        ws.stage = .none;
        ws.state = .idle;
        ws.active = .{};
        return err_mod.k_ra8_ok;
    }

    out_status.* = buildStatus(ws, ws.state);
    return err_mod.k_ra8_ok;
}

fn applyPowerAndYieldGate(ws: *Workspace, env: *const EnvState) bool {
    if (ws.policy.yield_to_reader != 0 and env.reader_active != 0) {
        ws.state = .yielded;
        ws.pause_reason = .reader_active;
        return true;
    }
    if (ws.policy.yield_to_display != 0 and env.display_busy != 0) {
        ws.state = .yielded;
        ws.pause_reason = .display_busy;
        return true;
    }
    if (env.charging == 0) {
        const thresh: u16 = if (ws.battery_paused)
            @min(@as(u16, 100), @as(u16, ws.policy.battery_min_pct) + ws.policy.battery_hysteresis_pct)
        else
            ws.policy.battery_min_pct;
        if (env.battery_pct < thresh) {
            ws.battery_paused = true;
            ws.state = .paused_battery;
            ws.pause_reason = .low_battery;
            return true;
        }
    }
    ws.battery_paused = false;
    return false;
}

fn activateNextQueued(ws: *Workspace) void {
    var best_idx: usize = 0;
    var i: usize = 1;
    while (i < ws.queue_len) : (i += 1) {
        if (beatsQueueEntry(ws.queue[i], ws.queue[best_idx])) {
            best_idx = i;
        }
    }
    const chosen = ws.queue[best_idx].spec;
    var j: usize = best_idx + 1;
    while (j < ws.queue_len) : (j += 1) {
        ws.queue[j - 1] = ws.queue[j];
    }
    ws.queue_len -= 1;

    ws.active = chosen;
    ws.has_active = true;
    ws.stage = .detect_ocr;
    ws.state = .running;
    ws.pause_reason = .none;
    ws.last_error = err_mod.k_ra8_ok;
    ws.resolved_lang = .unknown;
    ws.checkpoint_seq = 0;
    ws.pages_ocr_done = 0;
    ws.pages_translated_done = 0;
    ws.bubble_count = 0;
    ws.line_break_count = 0;
    ws.text_pool_bytes = 0;
    ws.tgl1_len = 0;
    ws.glossary_tbl = glossary.GlossaryTable.init(
        sidecar.Language.fromRaw(chosen.lang_hint) orelse .unknown,
        chosen.sidecar_key.glossary_revision,
    );
}

fn beatsQueueEntry(cand: QueueItem, cur: QueueItem) bool {
    if (cand.spec.unread != cur.spec.unread) return cand.spec.unread > cur.spec.unread;
    if (cand.spec.chapter_order != cur.spec.chapter_order) {
        return cand.spec.chapter_order < cur.spec.chapter_order;
    }
    return cand.seq < cur.seq;
}

fn advanceActiveStage(ws: *Workspace, hooks: *const Hooks) u16 {
    return switch (ws.stage) {
        .detect_ocr => runOcrPageStep(ws, hooks),
        .lang_glossary => runLangGlossaryStep(ws, hooks),
        .translate_fit => runTranslateFitPageStep(ws, hooks),
        .commit_sidecar => runCommitSidecarStep(ws, hooks),
        .none, .done => err_mod.k_ra8_ok,
    };
}

fn runOcrPageStep(ws: *Workspace, hooks: *const Hooks) u16 {
    const ocr_fn = hooks.ocr_page orelse return err_mod.toRa8Err(Error.NotSupported);
    if (ws.pages_ocr_done >= ws.active.page_count) {
        ws.stage = .lang_glossary;
        return err_mod.k_ra8_ok;
    }

    var pw: u16 = 0;
    var ph: u16 = 0;
    var bcount: u16 = 0;
    var text_len: usize = 0;
    const rc = ocr_fn(
        hooks.user,
        &ws.active,
        ws.pages_ocr_done,
        max_page_bubbles,
        &pw,
        &ph,
        &ws.stage_ocr_bubbles,
        &bcount,
        &ws.stage_text_a,
        ws.stage_text_a.len,
        &text_len,
    );
    if (rc != err_mod.k_ra8_ok) return rc;

    if (pw == 0 or ph == 0) return err_mod.toRa8Err(Error.ValidationFailed);
    if (bcount > max_page_bubbles or text_len > ws.stage_text_a.len) {
        return err_mod.toRa8Err(Error.InvalidSize);
    }
    if (@as(u32, ws.bubble_count) + bcount > max_bubbles) {
        return err_mod.toRa8Err(Error.InvalidSize);
    }

    const validated_text = ws.stage_text_a[0..text_len];
    const extra_bytes = ckpt.validateOcrBubbles(
        ws.stage_ocr_bubbles[0..bcount],
        validated_text,
        pw,
        ph,
    ) catch |e| return err_mod.toRa8Err(e);
    if (@as(u64, ws.text_pool_bytes) + extra_bytes > max_text_pool) {
        return err_mod.toRa8Err(Error.InvalidSize);
    }

    const first_bubble: u32 = ws.bubble_count;
    var pool_cursor = ws.text_pool_bytes;
    var i: usize = 0;
    while (i < bcount) : (i += 1) {
        const src = ws.stage_ocr_bubbles[i];
        const src_bytes = validated_text[src.source_off..][0..src.source_len];
        if (src_bytes.len > 0) {
            @memcpy(ws.text_pool[pool_cursor..][0..src_bytes.len], src_bytes);
        }
        ws.bubbles[ws.bubble_count] = .{
            .x = src.x,
            .y = src.y,
            .w = src.w,
            .h = src.h,
            .shape = src.shape,
            .text_orientation = src.text_orientation,
            .corner_radius = src.corner_radius,
            .reading_order = src.reading_order,
            .ocr_confidence_bps = src.ocr_confidence_bps,
            .font_size_px = 0,
            .line_count = 0,
            .first_line_break = 0,
            .source_text_off = pool_cursor,
            .source_text_len = src.source_len,
            .english_text_off = 0,
            .english_text_len = 0,
            .bubble_flags = 0,
            .reserved = .{ 0, 0, 0 },
        };
        ws.bubble_count += 1;
        pool_cursor += src.source_len;
    }

    ws.pages[ws.pages_ocr_done] = .{
        .page_index = ws.pages_ocr_done,
        .page_width = pw,
        .page_height = ph,
        .bubble_count = bcount,
        .first_bubble = first_bubble,
    };
    ws.text_pool_bytes = pool_cursor;
    ws.pages_ocr_done += 1;
    if (ws.pages_ocr_done == ws.active.page_count) {
        ws.stage = .lang_glossary;
    }

    return emitCheckpoint(ws, hooks);
}

fn runLangGlossaryStep(ws: *Workspace, hooks: *const Hooks) u16 {
    var counts = lang.ScriptCounts{};
    var bi: usize = 0;
    while (bi < ws.bubble_count) : (bi += 1) {
        const b = ws.bubbles[bi];
        const s = ws.text_pool[b.source_text_off..][0..b.source_text_len];
        ws.slice_ptrs[bi] = s;
        const c = lang.countScripts(s) catch |e| return err_mod.toRa8Err(e);
        counts.hangul_count +|= c.hangul_count;
        counts.kana_count +|= c.kana_count;
        counts.han_count +|= c.han_count;
        counts.other_count +|= c.other_count;
    }

    const hint = sidecar.Language.fromRaw(ws.active.lang_hint) orelse .unknown;
    const sel = lang.selectFromCounts(hint, counts);
    ws.resolved_lang = sidecar.Language.fromRaw(sel.language) orelse .unknown;
    if (ws.resolved_lang != .unknown) {
        ws.active.sidecar_key.source_lang = sel.language;
    }

    var loaded_existing = false;
    if (hooks.load_glossary) |load_fn| {
        var tgl_len: usize = 0;
        const rc = load_fn(hooks.user, ws.active.title_id, &ws.tgl1_buf, ws.tgl1_buf.len, &tgl_len);
        if (rc == err_mod.k_ra8_ok and tgl_len > 0) {
            if (tgl_len > ws.tgl1_buf.len) return err_mod.toRa8Err(Error.InvalidSize);
            _ = glossary.TglView.open(ws.tgl1_buf[0..tgl_len]) catch |e| return err_mod.toRa8Err(e);
            ws.tgl1_len = @intCast(tgl_len);
            loaded_existing = true;
        } else if (rc != err_mod.k_ra8_err_not_found and rc != err_mod.k_ra8_ok) {
            return rc;
        }
    }

    if (!loaded_existing) {
        const len = glossary.initEmptyWire(
            ws.resolved_lang,
            ws.active.sidecar_key.glossary_revision,
            &ws.tgl1_buf,
        ) catch |e| return err_mod.toRa8Err(e);
        ws.tgl1_len = @intCast(len);
    }

    ws.glossary_tbl.load(ws.tgl1_buf[0..ws.tgl1_len]) catch |e| return err_mod.toRa8Err(e);

    var added_terms: u32 = 0;
    if (ws.bubble_count > 0 and ws.resolved_lang != .unknown) {
        if (hooks.translate_term) |term_fn| {
            const cand_count = glossary.extractCandidates(
                ws.resolved_lang,
                &ws.glossary_tbl,
                ws.slice_ptrs[0..ws.bubble_count],
                &ws.candidates,
            ) catch |e| return err_mod.toRa8Err(e);

            var ci: usize = 0;
            while (ci < cand_count) : (ci += 1) {
                const cand = ws.candidates[ci];
                var eng_len: usize = 0;
                const rc = term_fn(
                    hooks.user,
                    @backingInt(ws.resolved_lang),
                    cand.stem_slice.ptr,
                    cand.stem_slice.len,
                    &ws.stage_text_a,
                    ws.stage_text_a.len,
                    &eng_len,
                );
                if (rc != err_mod.k_ra8_ok) return rc;
                if (eng_len == 0 or eng_len > ws.stage_text_a.len) continue;
                const eng_slice = ws.stage_text_a[0..eng_len];
                const new_len = glossary.upsertWire(
                    ws.tgl1_buf[0..ws.tgl1_len],
                    cand.stem_slice,
                    eng_slice,
                    &ws.tgl1_scratch,
                ) catch |e| switch (e) {
                    Error.InvalidSize => break,
                    else => return err_mod.toRa8Err(e),
                };
                @memcpy(ws.tgl1_buf[0..new_len], ws.tgl1_scratch[0..new_len]);
                ws.tgl1_len = @intCast(new_len);
                added_terms += 1;
            }
            if (added_terms > 0) {
                ws.glossary_tbl.load(ws.tgl1_buf[0..ws.tgl1_len]) catch |e| return err_mod.toRa8Err(e);
                ws.active.sidecar_key.glossary_revision = ws.glossary_tbl.revision;
            }
        }
    }

    ws.stage = .translate_fit;
    return emitCheckpoint(ws, hooks);
}

fn runTranslateFitPageStep(ws: *Workspace, hooks: *const Hooks) u16 {
    if (ws.pages_translated_done >= ws.pages_ocr_done) {
        ws.stage = .commit_sidecar;
        return err_mod.k_ra8_ok;
    }

    const paint = hooks.paint orelse return err_mod.toRa8Err(Error.NotSupported);
    const page = ws.pages[ws.pages_translated_done];
    if (page.bubble_count > 0 and hooks.translate_batch == null) {
        return err_mod.toRa8Err(Error.NotSupported);
    }

    const saved_pool = ws.text_pool_bytes;
    const saved_breaks = ws.line_break_count;
    const rc = translateAndFitPageBubbles(ws, hooks, paint, page);
    if (rc != err_mod.k_ra8_ok) {
        ws.text_pool_bytes = saved_pool;
        ws.line_break_count = saved_breaks;
        resetPageFitFields(ws, page);
        return rc;
    }

    ws.pages_translated_done += 1;
    if (ws.pages_translated_done == ws.active.page_count) {
        ws.stage = .commit_sidecar;
    }
    return emitCheckpoint(ws, hooks);
}

fn translateAndFitPageBubbles(
    ws: *Workspace,
    hooks: *const Hooks,
    paint: *const overlay.Paint,
    page: sidecar.PageEntry,
) u16 {
    if (page.bubble_count == 0) return err_mod.k_ra8_ok;
    const batch_fn = hooks.translate_batch orelse return err_mod.toRa8Err(Error.NotSupported);
    const tgl_view = glossary.TglView.open(ws.tgl1_buf[0..ws.tgl1_len]) catch |e| return err_mod.toRa8Err(e);
    const nmt_model = @backingInt(lang.nmtModelFor(ws.resolved_lang));
    const batch_lim: usize = if (ws.policy.nmt_batch_max == 0) default_batch else ws.policy.nmt_batch_max;

    var offset: usize = 0;
    while (offset < page.bubble_count) {
        const count: usize = @min(batch_lim, @as(usize, page.bubble_count) - offset);
        var src_spans: [max_batch]Span = undefined;
        var eng_spans: [max_batch]Span = undefined;
        var masked_total: usize = 0;

        var k: usize = 0;
        while (k < count) : (k += 1) {
            const b = ws.bubbles[@as(usize, page.first_bubble) + offset + k];
            const src_utf8 = ws.text_pool[b.source_text_off..][0..b.source_text_len];
            const mlen = tgl_view.maskText(
                src_utf8,
                ws.stage_text_a[masked_total..],
                &ws.stage_plans[k],
            ) catch |e| return err_mod.toRa8Err(e);
            src_spans[k] = .{ .off = @intCast(masked_total), .len = @intCast(mlen) };
            masked_total += mlen;
        }

        const rc = batch_fn(
            hooks.user,
            nmt_model,
            &ws.stage_text_a,
            masked_total,
            &src_spans,
            @intCast(count),
            &ws.stage_text_b,
            ws.stage_text_b.len,
            &eng_spans,
        );
        if (rc != err_mod.k_ra8_ok) return rc;

        k = 0;
        while (k < count) : (k += 1) {
            const b_idx = @as(usize, page.first_bubble) + offset + k;
            const span = eng_spans[k];
            const span_end: u64 = @as(u64, span.off) + span.len;
            if (span_end > ws.stage_text_b.len) return err_mod.toRa8Err(Error.ValidationFailed);
            const masked_eng = ws.stage_text_b[span.off..][0..span.len];
            const eng_len = tgl_view.restoreText(
                &ws.stage_plans[k],
                masked_eng,
                &ws.stage_restore,
            ) catch |e| return err_mod.toRa8Err(e);
            const eng_utf8 = ws.stage_restore[0..eng_len];

            const b_ptr = &ws.bubbles[b_idx];
            var breaks_tmp: [overlay.max_lines]u16 = undefined;
            const fit = overlay.fitText(
                paint,
                .{
                    .x = b_ptr.x,
                    .y = b_ptr.y,
                    .w = b_ptr.w,
                    .h = b_ptr.h,
                    .shape = b_ptr.shape,
                    .text_orientation = b_ptr.text_orientation,
                    .corner_radius = b_ptr.corner_radius,
                },
                ws.policy.overlay_style,
                eng_utf8,
                &breaks_tmp,
            ) catch |e| return err_mod.toRa8Err(e);

            if (@as(u64, ws.text_pool_bytes) + eng_len > max_text_pool) {
                return err_mod.toRa8Err(Error.InvalidSize);
            }
            if (@as(u64, ws.line_break_count) + fit.line_count > max_line_breaks) {
                return err_mod.toRa8Err(Error.InvalidSize);
            }

            const eng_off = ws.text_pool_bytes;
            if (eng_len > 0) {
                @memcpy(ws.text_pool[eng_off..][0..eng_len], eng_utf8);
                ws.text_pool_bytes += @intCast(eng_len);
            }
            const first_brk = ws.line_break_count;
            if (fit.line_count > 0) {
                @memcpy(ws.line_breaks[first_brk..][0..fit.line_count], breaks_tmp[0..fit.line_count]);
                ws.line_break_count += fit.line_count;
            }
            b_ptr.english_text_off = eng_off;
            b_ptr.english_text_len = @intCast(eng_len);
            b_ptr.font_size_px = fit.font_size_px;
            b_ptr.line_count = fit.line_count;
            b_ptr.first_line_break = first_brk;
        }
        offset += count;
    }
    return err_mod.k_ra8_ok;
}

fn resetPageFitFields(ws: *Workspace, page: sidecar.PageEntry) void {
    var i: usize = 0;
    while (i < page.bubble_count) : (i += 1) {
        const b = &ws.bubbles[@as(usize, page.first_bubble) + i];
        b.english_text_off = 0;
        b.english_text_len = 0;
        b.font_size_px = 0;
        b.line_count = 0;
        b.first_line_break = 0;
    }
}

fn runCommitSidecarStep(ws: *Workspace, hooks: *const Hooks) u16 {
    const commit_fn = hooks.commit_chapter orelse return err_mod.toRa8Err(Error.NotSupported);
    const trn1_len = sidecar.writeSidecar(
        &ws.active.sidecar_key,
        ws.pages[0..ws.pages_ocr_done],
        ws.bubbles[0..ws.bubble_count],
        ws.line_breaks[0..ws.line_break_count],
        ws.text_pool[0..ws.text_pool_bytes],
        &ws.io_buf,
    ) catch |e| return err_mod.toRa8Err(e);

    const rc = commit_fn(
        hooks.user,
        &ws.active,
        &ws.io_buf,
        trn1_len,
        &ws.tgl1_buf,
        ws.tgl1_len,
    );
    if (rc != err_mod.k_ra8_ok) return rc;

    if (hooks.save_checkpoint) |save_fn| {
        const empty: [1]u8 = .{0};
        const clr_rc = save_fn(hooks.user, &empty, 0);
        if (clr_rc != err_mod.k_ra8_ok) return clr_rc;
    }

    ws.has_active = false;
    ws.stage = .done;
    ws.state = .completed;
    return err_mod.k_ra8_ok;
}

fn emitCheckpoint(ws: *Workspace, hooks: *const Hooks) u16 {
    ws.checkpoint_seq +|= 1;
    const save_fn = hooks.save_checkpoint orelse return err_mod.k_ra8_ok;
    const len = ckpt.serializeCheckpoint(.{
        .chapter = &ws.active,
        .stage = ws.stage,
        .resolved_lang = ws.resolved_lang,
        .checkpoint_seq = ws.checkpoint_seq,
        .pages_ocr_done = ws.pages_ocr_done,
        .pages_translated_done = ws.pages_translated_done,
        .pages = ws.pages[0..ws.pages_ocr_done],
        .bubbles = ws.bubbles[0..ws.bubble_count],
        .line_breaks = ws.line_breaks[0..ws.line_break_count],
        .text_pool = ws.text_pool[0..ws.text_pool_bytes],
        .tgl1_bytes = ws.tgl1_buf[0..ws.tgl1_len],
    }, &ws.io_buf) catch |e| return err_mod.toRa8Err(e);

    return save_fn(hooks.user, &ws.io_buf, len);
}

fn clearActiveChapter(ws: *Workspace) void {
    ws.has_active = false;
    ws.state = .idle;
    ws.stage = .none;
    ws.pause_reason = .none;
    ws.last_error = err_mod.k_ra8_ok;
    ws.active = .{};
    ws.pages_ocr_done = 0;
    ws.pages_translated_done = 0;
    ws.bubble_count = 0;
    ws.line_break_count = 0;
    ws.text_pool_bytes = 0;
    ws.tgl1_len = 0;
}

fn buildStatus(ws: *const Workspace, state: State) Status {
    return statusSnapshot(ws, ws.has_active, state, ws.stage, ws.pause_reason, ws.last_error);
}

fn buildCompletedSnapshot(ws: *const Workspace) Status {
    return statusSnapshot(ws, false, .completed, .done, .none, err_mod.k_ra8_ok);
}

fn statusSnapshot(
    ws: *const Workspace,
    active_flag: bool,
    state: State,
    stage: Stage,
    pause_reason: PauseReason,
    last_error: u16,
) Status {
    const keep_ids = active_flag or state == .completed;
    return .{
        .active_title_id = if (keep_ids) ws.active.title_id else 0,
        .active_chapter_order = if (keep_ids) ws.active.chapter_order else 0,
        .checkpoint_seq = ws.checkpoint_seq,
        .queued_count = ws.queue_len,
        .page_count = if (keep_ids) ws.active.page_count else 0,
        .pages_ocr_done = ws.pages_ocr_done,
        .pages_translated_done = ws.pages_translated_done,
        .last_error = last_error,
        .state = @backingInt(state),
        .stage = @backingInt(stage),
        .pause_reason = @backingInt(pause_reason),
        .resolved_lang = @backingInt(ws.resolved_lang),
        .has_active = if (active_flag) 1 else 0,
        .reserved0 = .{ 0, 0, 0, 0, 0 },
    };
}

fn sameChapter(a: *const ChapterSpec, b: *const ChapterSpec) bool {
    return a.title_id == b.title_id and a.chapter_order == b.chapter_order;
}

fn removeMatchingQueueEntry(ws: *Workspace, target: *const ChapterSpec) void {
    var i: usize = 0;
    while (i < ws.queue_len) : (i += 1) {
        if (sameChapter(&ws.queue[i].spec, target)) {
            var j: usize = i + 1;
            while (j < ws.queue_len) : (j += 1) {
                ws.queue[j - 1] = ws.queue[j];
            }
            ws.queue_len -= 1;
            return;
        }
    }
}

const allZero = ckpt.allZero;
