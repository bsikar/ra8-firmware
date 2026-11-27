//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Multilingual manga/webtoon evaluation corpus and optional emulator trace parser.

const std = @import("std");
const metrics = @import("metrics.zig");

/// Maximum bytes read from an external corpus or emulator trace file.
pub const max_json_bytes: usize = 16 * 1024 * 1024;

/// Embedded default license-clean evaluation corpus (pure 7-bit ASCII JSON).
pub const default_corpus_json: []const u8 = @embedFile("eval_corpus");

/// Detector activation region in detector feature-map coordinates.
pub const DetRegion = struct {
    x: u16,
    y: u16,
    w: u16,
    h: u16,
    prob: f32,
    is_furigana: bool = false,
};

/// Ground-truth speech or narration bubble plus deterministic quantization stress inputs.
pub const BubbleSpec = struct {
    x: u16,
    y: u16,
    w: u16,
    h: u16,
    orientation: []const u8,
    source_text: []const u8,
    english_ref: []const u8,
    ocr_margin: f32,
    ocr_distractor_sub: []const u8 = "",
    nmt_quant_margin: f32,
    nmt_alt_token: []const u8 = "",

    pub fn box(self: BubbleSpec) metrics.Box {
        return .{ .x = self.x, .y = self.y, .w = self.w, .h = self.h };
    }
};

/// Single labeled manga/webtoon page in the evaluation set.
pub const PageSpec = struct {
    page_id: []const u8,
    lang: []const u8,
    layout: []const u8,
    page_width: u16,
    page_height: u16,
    det_map_width: u16,
    det_map_height: u16,
    det_regions: []const DetRegion,
    bubbles: []const BubbleSpec,
};

/// Top-level corpus specification parsed from JSON.
pub const Corpus = struct {
    schema: u32,
    license: []const u8,
    provenance: []const u8,
    languages: []const []const u8,
    pages: []const PageSpec,
};

/// Predicted box + OCR/English strings for one page from an external emulator run.
pub const EmuBubblePred = struct {
    source_text: []const u8,
    english_text: []const u8,
};

pub const EmuPagePred = struct {
    page_id: []const u8,
    boxes: []const metrics.Box,
    bubbles: []const EmuBubblePred,
    furigana_dropped: u32 = 0,
    weight_bytes: u64 = 0,
};

pub const EmuTrace = struct {
    schema: u32 = 1,
    pages: []const EmuPagePred,
};

/// Parse `Corpus` JSON and validate required schema version and non-empty slices.
pub fn parseCorpus(allocator: std.mem.Allocator, bytes: []const u8) !std.json.Parsed(Corpus) {
    const parsed = try std.json.parseFromSlice(Corpus, allocator, bytes, .{
        .ignore_unknown_fields = false,
    });
    errdefer parsed.deinit();

    if (parsed.value.schema != 1 or parsed.value.pages.len == 0) {
        return error.InvalidCorpusSchema;
    }
    for (parsed.value.pages) |page| {
        if (page.det_map_width == 0 or page.det_map_height == 0 or
            page.page_width == 0 or page.page_height == 0 or
            page.bubbles.len == 0)
        {
            return error.InvalidCorpusPage;
        }
    }
    return parsed;
}

/// Parse external emulator prediction trace JSON.
pub fn parseEmuTrace(allocator: std.mem.Allocator, bytes: []const u8) !std.json.Parsed(EmuTrace) {
    const parsed = try std.json.parseFromSlice(EmuTrace, allocator, bytes, .{
        .ignore_unknown_fields = false,
    });
    errdefer parsed.deinit();

    if (parsed.value.schema != 1) return error.InvalidEmuTraceSchema;
    return parsed;
}
