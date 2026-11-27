//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Unit and integration tests for `marian_zig_runner`.

const std = @import("std");
const runner = @import("runner");
const tool_options = @import("tool_options");

fn writeU16(buf: []u8, off: usize, v: u16) void {
    std.mem.writeInt(u16, buf[off..][0..2], v, .little);
}

fn writeU32(buf: []u8, off: usize, v: u32) void {
    std.mem.writeInt(u32, buf[off..][0..4], v, .little);
}

fn writeI8(buf: []u8, off: usize, v: i8) void {
    buf[off] = @bitCast(v);
}

fn fillTinyTvocEntries(
    out: []u8,
    texts: []const []const u8,
    kinds: []const u8,
    scores: []const f32,
    perm_off: usize,
    pool_off: usize,
) void {
    var p_off: usize = 0;
    for (texts, 0..) |t, i| {
        const rec = 56 + i * 12;
        writeU32(out, rec + 0, @intCast(p_off));
        writeU16(out, rec + 4, @intCast(t.len));
        out[rec + 6] = kinds[i];
        writeU32(out, rec + 8, @bitCast(scores[i]));
        @memcpy(out[pool_off + p_off .. pool_off + p_off + t.len], t);
        p_off += t.len;
    }
    const perm = [_]u32{ 0, 1, 5, 3, 2, 4 };
    for (perm, 0..) |pid, i| writeU32(out, perm_off + i * 4, pid);
}

fn buildTinyTvoc(allocator: std.mem.Allocator) ![]u8 {
    // 6-piece Unigram vocab: 0=</s>(ctrl), 1=<unk>(unk), 2=<sp>hi, 3=<sp>go, 4=<sp>ok, 5=<sp>bye
    const metaspace = "\xE2\x96\x81";
    const texts = [_][]const u8{
        "</s>", "<unk>", metaspace ++ "hi", metaspace ++ "go", metaspace ++ "ok", metaspace ++ "bye",
    };
    const kinds = [_]u8{ 2, 1, 0, 0, 0, 0 };
    const scores = [_]f32{ 0.0, 0.0, -1.0, -2.0, -1.5, -2.5 };

    var pool_len: usize = 0;
    for (texts) |t| pool_len += t.len;
    const perm_off = 56 + texts.len * 12;
    const pool_off = perm_off + texts.len * 4;
    const total = pool_off + pool_len;
    const out = try allocator.alloc(u8, total);
    @memset(out, 0);

    @memcpy(out[0..4], "TVOC");
    writeU16(out, 4, 1);
    out[6] = 0;
    out[7] = 0x01 | 0x02 | 0x04;
    writeU32(out, 8, @intCast(total));
    writeU32(out, 16, texts.len);
    writeU32(out, 20, 1);
    writeU32(out, 24, 0xFFFF_FFFF);
    writeU32(out, 28, 0);
    writeU32(out, 32, 0xFFFF_FFFF);
    writeU32(out, 36, @bitCast(@as(f32, -2.5)));
    writeU32(out, 40, 56);
    writeU32(out, 44, perm_off);
    writeU32(out, 48, pool_off);
    writeU32(out, 52, 0);

    fillTinyTvocEntries(out, &texts, &kinds, &scores, perm_off, pool_off);
    writeU32(out, 12, std.hash.Crc32.hash(out));
    return out;
}

const TinySpec = struct {
    slot: usize,
    dtype: u8,
    rows: u32,
    cols: u32,
    scale: f32,
    vals_i8: []const i8 = &.{},
    vals_f32: []const f32 = &.{},
};

const tiny_global_specs = [_]TinySpec{
    // embed(4)=[2,0]; step 0 uses zero_bos+pos(0)=[0,1] -> LN=[-1,1] -> row 4 ([0,5]) wins;
    // step 1 uses embed(4)+pos(1) -> LN=[1,-1] -> row 0 ([5,0], EOS) wins.
    .{ .slot = 0, .dtype = 1, .rows = 6, .cols = 2, .scale = 1.0, .vals_i8 = &.{ 1, 0, 0, 0, 0, 1, 0, 0, 2, 0, 0, 0 } },
    .{ .slot = 2, .dtype = 1, .rows = 6, .cols = 2, .scale = 1.0, .vals_i8 = &.{ 5, 0, 0, 0, 0, 0, 0, 0, 0, 5, 0, 0 } },
    .{ .slot = 3, .dtype = 2, .rows = 6, .cols = 1, .scale = 1.0, .vals_f32 = &.{ 0.0, -5.0, -5.0, -5.0, 0.0, -5.0 } },
    .{ .slot = 4, .dtype = 2, .rows = 2, .cols = 1, .scale = 1.0, .vals_f32 = &.{ 1.0, 1.0 } },
    .{ .slot = 5, .dtype = 2, .rows = 2, .cols = 1, .scale = 1.0, .vals_f32 = &.{ 0.0, 0.0 } },
    .{ .slot = 6, .dtype = 2, .rows = 2, .cols = 1, .scale = 1.0, .vals_f32 = &.{ 1.0, 1.0 } },
    .{ .slot = 7, .dtype = 2, .rows = 2, .cols = 1, .scale = 1.0, .vals_f32 = &.{ 0.0, 0.0 } },
};

const TinySlotKind = enum { mat_i8, bias_f32, ln_scale_f32 };

fn tinyAttnFfnKind(sub: usize) TinySlotKind {
    return switch (sub) {
        8, 14 => .ln_scale_f32,
        1, 3, 5, 7, 9, 11, 13, 15 => .bias_f32,
        else => .mat_i8,
    };
}

fn tinyLayerSlotKind(slot: usize) TinySlotKind {
    if (slot < 24) return tinyAttnFfnKind(slot - 8);
    const d = slot - 24;
    if (d <= 1) return .mat_i8;
    if (d == 2 or d == 4) return .bias_f32;
    if (d == 3) return .ln_scale_f32;
    return tinyAttnFfnKind(d - 5);
}

fn writeGlobalTinySlot(out: []u8, d_off: usize, sp: TinySpec, cursor: usize) usize {
    const raw_len = if (sp.dtype == 1) sp.vals_i8.len else sp.vals_f32.len * 4;
    writeU32(out, d_off + 0, @intCast(cursor));
    writeU32(out, d_off + 4, @intCast(raw_len));
    writeU32(out, d_off + 8, sp.rows);
    writeU32(out, d_off + 12, sp.cols);
    writeU32(out, d_off + 16, @bitCast(sp.scale));
    out[d_off + 20] = sp.dtype;
    if (sp.dtype == 1) {
        for (sp.vals_i8, 0..) |v, j| writeI8(out, cursor + j, v);
    } else {
        for (sp.vals_f32, 0..) |v, j| writeU32(out, cursor + j * 4, @bitCast(v));
    }
    return cursor + std.mem.alignForward(usize, raw_len, 4);
}

fn writeTinyTmntDescriptors(out: []u8, weights_at: usize) usize {
    var cursor = weights_at;
    for (0..45) |slot| {
        const d_off = 84 + slot * 24;
        if (slot == 1) {
            out[d_off + 20] = 0;
            continue;
        }
        var found: ?TinySpec = null;
        for (tiny_global_specs) |sp| {
            if (sp.slot == slot) found = sp;
        }
        if (found) |sp| {
            cursor = writeGlobalTinySlot(out, d_off, sp, cursor);
            continue;
        }
        const kind = tinyLayerSlotKind(slot);
        const is_vec = kind != .mat_i8;
        const byte_len: usize = if (is_vec) 8 else 4;
        writeU32(out, d_off + 0, @intCast(cursor));
        writeU32(out, d_off + 4, @intCast(byte_len));
        writeU32(out, d_off + 8, 2);
        writeU32(out, d_off + 12, if (is_vec) 1 else 2);
        writeU32(out, d_off + 16, @bitCast(@as(f32, 1.0)));
        out[d_off + 20] = if (is_vec) 2 else 1;
        if (kind == .ln_scale_f32) {
            writeU32(out, cursor, @bitCast(@as(f32, 1.0)));
            writeU32(out, cursor + 4, @bitCast(@as(f32, 1.0)));
        }
        cursor += std.mem.alignForward(usize, byte_len, 4);
    }
    return cursor;
}

fn buildTinySsruTmnt(allocator: std.mem.Allocator) ![]u8 {
    // vocab=6, d=2, d_ff=2, max_seq=4, enc=1, dec=1 (SSRU: 8 + 16 + 21 = 45 slots).
    const dir_end: usize = 84 + 45 * 24;
    const weights_at = std.mem.alignForward(usize, dir_end, 4);
    const max_payload: usize = 45 * 24;
    const sl_bytes: usize = 16 + (6 + 1 + 1) * 4;
    const tmp = try allocator.alloc(u8, weights_at + max_payload + sl_bytes);
    errdefer allocator.free(tmp);
    @memset(tmp, 0);

    const sl_off = writeTinyTmntDescriptors(tmp, weights_at);
    const total = sl_off + sl_bytes;
    const out = try allocator.realloc(tmp, total);

    @memcpy(out[0..4], "TMNT");
    writeU16(out, 4, 1);
    writeU16(out, 6, runner.c.marian_flag_decoder_ssru | runner.c.marian_flag_zero_bos);
    writeU32(out, 8, @intCast(total));
    out[20] = 1; // Korean NMT model ID
    out[21] = 1;
    out[22] = 1;
    out[23] = 1;
    writeU16(out, 24, 2);
    writeU16(out, 26, 2);
    writeU16(out, 28, 4);
    writeU32(out, 32, 6);
    writeU32(out, 36, 1);
    writeU32(out, 40, 0);
    writeU32(out, 44, 0);
    writeU32(out, 48, 0);
    writeU32(out, 52, @bitCast(@as(f32, 1e-6)));
    writeU32(out, 64, @intCast(sl_off));
    writeU32(out, 68, sl_bytes);
    writeU32(out, 72, 45);
    writeU32(out, 76, @intCast(weights_at));
    writeU32(out, 80, @intCast(sl_off - weights_at));

    @memcpy(out[sl_off .. sl_off + 4], "TSLT");
    writeU16(out, sl_off + 4, 1);
    writeU16(out, sl_off + 6, 1);
    writeU32(out, sl_off + 8, 6);
    writeU32(out, sl_off + 12, 1);
    for (0..7) |i| writeU32(out, sl_off + 16 + i * 4, if (i <= 2) 0 else 1);
    writeU32(out, sl_off + 16 + 7 * 4, 4);

    writeU32(out, 16, std.hash.Crc32.hash(out[weights_at..]));
    writeU32(out, 12, 0);
    writeU32(out, 12, std.hash.Crc32.hash(out[0..dir_end]));
    return out;
}

test "runner decodes tiny SSRU TMNT+TVOC+TSLT and runs batch parity" {
    const alloc = std.testing.allocator;
    const tvoc = try buildTinyTvoc(alloc);
    defer alloc.free(tvoc);
    const tmnt = try buildTinySsruTmnt(alloc);
    defer alloc.free(tmnt);

    const sentences = [_][]const u8{"hi"};
    var report = try runner.evaluateSentences(alloc, tmnt, tvoc, &sentences, 4, 3, true, true);
    defer report.deinit();

    try std.testing.expectEqual(@as(usize, 1), report.cases.len);
    try std.testing.expect(report.cases[0].greedy_ids.len >= 1);
    try std.testing.expectEqual(@as(u32, 4), report.cases[0].greedy_ids[0]);
    try std.testing.expectEqual(@as(u32, 0), report.cases[0].greedy_ids[1]);
    try std.testing.expect(report.cases[0].stopped_on_eos);
    try std.testing.expect(report.batch_full != null);
    try std.testing.expect(report.batch_shortlist != null);
    try std.testing.expectEqualSlices(
        u32,
        report.cases[0].greedy_ids,
        report.batch_shortlist.?.cases[0].greedy_ids,
    );
}

test "runner CLI binary responds cleanly to --help" {
    const alloc = std.testing.allocator;
    const io = std.testing.io;
    const run = try std.process.run(alloc, io, .{
        .argv = &.{ tool_options.tool_exe, "--help" },
    });
    defer alloc.free(run.stdout);
    defer alloc.free(run.stderr);

    try std.testing.expectEqual(@as(u8, 0), run.term.exited);
    try std.testing.expect(std.mem.indexOf(u8, run.stdout, "usage: marian_zig_runner") != null);
}
