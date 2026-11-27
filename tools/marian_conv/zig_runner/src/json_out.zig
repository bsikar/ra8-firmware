//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Deterministic pure 7-bit ASCII JSON serializer for `RunReport`.

const std = @import("std");
const runner = @import("runner.zig");

pub fn writeJsonEscapedString(out: *std.Io.Writer, text: []const u8) !void {
    try out.writeByte('"');
    var view = std.unicode.Utf8View.initUnchecked(text);
    var it = view.iterator();
    while (it.nextCodepoint()) |cp| {
        switch (cp) {
            '"' => try out.writeAll("\\\""),
            '\\' => try out.writeAll("\\\\"),
            '\n' => try out.writeAll("\\n"),
            '\r' => try out.writeAll("\\r"),
            '\t' => try out.writeAll("\\t"),
            0x20...0x21, 0x23...0x5B, 0x5D...0x7E => try out.writeByte(@intCast(cp)),
            else => {
                if (cp < 0x10000) {
                    try out.print("\\u{x:0>4}", .{cp});
                } else {
                    const adj = cp - 0x10000;
                    const hi: u16 = @intCast(0xD800 + (adj >> 10));
                    const lo: u16 = @intCast(0xDC00 + (adj & 0x3FF));
                    try out.print("\\u{x:0>4}\\u{x:0>4}", .{ hi, lo });
                }
            },
        }
    }
    try out.writeByte('"');
}

fn writeU32Array(out: *std.Io.Writer, vals: []const u32) !void {
    try out.writeByte('[');
    for (vals, 0..) |v, idx| {
        if (idx > 0) try out.writeByte(',');
        try out.print("{d}", .{v});
    }
    try out.writeByte(']');
}

fn writeStep(out: *std.Io.Writer, s: runner.StepTrace) !void {
    try out.print(
        "{{\"step\":{d},\"selected_id\":{d},\"selected_logit\":{d:.6}," ++
            "\"selected_logit_bits\":\"0x{x:0>8}\",\"logits_sha256\":\"{s}\",\"top_candidates\":[",
        .{ s.step, s.selected_id, s.selected_logit, s.selected_logit_bits, s.logits_sha256 },
    );
    for (s.top_candidates, 0..) |cand, c_idx| {
        if (c_idx > 0) try out.writeByte(',');
        try out.print("{{\"id\":{d},\"logit\":{d:.6}}}", .{ cand.id, cand.logit });
    }
    try out.writeAll("]}");
}

fn writeCase(out: *std.Io.Writer, case: runner.SentenceTrace) !void {
    try out.writeAll("{\"source_text\":");
    try writeJsonEscapedString(out, case.source_text);
    try out.writeAll(",\"src_ids\":");
    try writeU32Array(out, case.src_ids);
    try out.writeAll(",\"greedy_ids\":");
    try writeU32Array(out, case.greedy_ids);
    try out.writeAll(",\"decoded_en\":");
    try writeJsonEscapedString(out, case.decoded_en);
    try out.print(
        ",\"stopped_on_eos\":{},\"weight_bytes_read\":{d},\"steps\":[",
        .{ case.stopped_on_eos, case.weight_bytes_read },
    );
    for (case.steps, 0..) |s, s_idx| {
        if (s_idx > 0) try out.writeByte(',');
        try writeStep(out, s);
    }
    try out.writeAll("]}");
}

fn writeBatchSummary(out: *std.Io.Writer, name: []const u8, summary: runner.BatchSummary) !void {
    try out.print(
        ",\"{s}\":{{\"completed_jobs\":{d},\"decoder_steps\":{d},\"total_weight_bytes_read\":{d},\"cases\":[",
        .{ name, summary.completed_jobs, summary.decoder_steps, summary.total_weight_bytes_read },
    );
    for (summary.cases, 0..) |item, idx| {
        if (idx > 0) try out.writeByte(',');
        try out.writeAll("{\"greedy_ids\":");
        try writeU32Array(out, item.greedy_ids);
        try out.print(",\"stopped_on_eos\":{}}}", .{item.stopped_on_eos});
    }
    try out.writeAll("]}");
}

pub fn writeReport(out: *std.Io.Writer, report: runner.RunReport) !void {
    try out.print(
        "{{\"vocab_size\":{d},\"d_model\":{d},\"d_ff\":{d},\"num_enc_layers\":{d}," ++
            "\"num_dec_layers\":{d},\"model_flags\":{d},\"shortlist_bytes\":{d},\"cases\":[",
        .{
            report.marian.vocab_size,
            report.marian.d_model,
            report.marian.d_ff,
            report.marian.num_enc_layers,
            report.marian.num_dec_layers,
            report.marian.model_flags,
            report.marian.shortlist_bytes,
        },
    );
    for (report.cases, 0..) |case, idx| {
        if (idx > 0) try out.writeByte(',');
        try writeCase(out, case);
    }
    try out.writeByte(']');
    if (report.batch_full) |bf| try writeBatchSummary(out, "batch_full", bf);
    if (report.batch_shortlist) |bs| try writeBatchSummary(out, "batch_shortlist", bs);
    try out.writeAll("}\n");
}
