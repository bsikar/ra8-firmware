//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Caller-workspace two-spool JOF round-trip verification and optional PPM dump.

const std = @import("std");
const c_abi = @import("c_abi");
const fmt_report = @import("fmt_report");
const source_io = @import("source_io");

const max_diffs: u32 = 8;
const hex_digits = "0123456789ABCDEF";

const Atlases = struct {
    reference: c_abi.JofInfo,
    banded: c_abi.JofInfo,
};

const CompareState = struct {
    need: *const c_abi.JofVerifyRequirements,
    work: *c_abi.JofVerifyWorkspace,
    ref: *const c_abi.Spool,
    got: *const c_abi.Spool,
    rinfo: *const c_abi.JofInfo,
    ginfo: *const c_abi.JofInfo,
    dump: ?*c_abi.Transaction,
    report: *const c_abi.Sink,
    diffs: u64 = 0,
    shown: u32 = 0,
    dump_ok: bool = false,
};

fn spoolAppendCallback(
    ctx: ?*anyopaque,
    bytes: ?[*]const u8,
    len: usize,
) callconv(.c) c_abi.Ra8Err {
    const spool: *c_abi.Spool = @ptrCast(@alignCast(ctx orelse return c_abi.k_ra8_err_null_ptr));
    const append_fn = spool.append orelse return c_abi.k_ra8_err_null_ptr;
    return append_fn(spool.ctx, bytes, len);
}

fn produceAtlas(
    comptime deps: c_abi.Deps,
    source: *const c_abi.Source,
    need: *const c_abi.JofVerifyRequirements,
    tile_h: u16,
    work: *c_abi.JofVerifyWorkspace,
    spool: *c_abi.Spool,
    info: *c_abi.JofInfo,
) c_abi.Ra8Err {
    var pull = source_io.PullCursor{ .source = source, .offset = 0 };
    const cfg = c_abi.JofProduceCfg{
        .pull = source_io.pullCallback,
        .pull_ctx = &pull,
        .sink = spoolAppendCallback,
        .sink_ctx = spool,
        .tile_w = need.width,
        .tile_h = tile_h,
        .codec = c_abi.k_jof_codec_deflate,
        .max_width = need.width,
        .max_height = need.height,
        .work = work.work,
        .work_cap = work.work_cap,
        .webp_work = work.webp_work,
        .webp_work_cap = work.webp_work_cap,
    };

    var rc = source_io.checkStable(source);
    if (rc == c_abi.k_ra8_ok) rc = deps.jof_produce(&cfg, info);
    if (rc == c_abi.k_ra8_ok) rc = source_io.checkStable(source);
    if (rc == c_abi.k_ra8_ok) rc = spool.seal.?(spool.ctx, info.total_size);
    return rc;
}

fn infoMatchesPolicy(
    info: *const c_abi.JofInfo,
    need: *const c_abi.JofVerifyRequirements,
    tile_h: u16,
) bool {
    const rows_u32 = (@as(u32, need.height) + tile_h - 1) / tile_h;
    if (rows_u32 > std.math.maxInt(u16)) return false;
    const rows: u16 = @intCast(rows_u32);
    return info.width == need.width and
        info.height == need.height and
        info.tile_w == need.width and
        info.tile_h == tile_h and
        info.tile_cols == 1 and
        info.tile_rows == rows and
        info.tile_count == rows and
        info.bpp == need.bpp and
        info.codec == c_abi.k_jof_codec_deflate;
}

fn preflightAtlas(
    comptime deps: c_abi.Deps,
    spool: *const c_abi.Spool,
    info: *const c_abi.JofInfo,
    work: *c_abi.JofVerifyWorkspace,
    reference: bool,
) c_abi.Ra8Err {
    const buf = if (reference) work.row else work.band_tile;
    const cap = if (reference) work.row_cap else work.band_tile_cap;
    var y: u16 = 0;
    while (y < info.tile_rows) : (y += 1) {
        var out_w: u16 = 0;
        var out_h: u16 = 0;
        const rc = deps.jof_read_tile(
            spool.read_at,
            spool.ctx,
            info,
            0,
            y,
            work.scratch,
            work.scratch_cap,
            buf,
            cap,
            &out_w,
            &out_h,
        );
        if (rc != c_abi.k_ra8_ok or out_w != info.width or (reference and out_h != 1)) {
            return if (rc == c_abi.k_ra8_ok) c_abi.k_ra8_err_validation_failed else rc;
        }
    }
    return c_abi.k_ra8_ok;
}

fn acceptAtlas(
    comptime deps: c_abi.Deps,
    spool: *const c_abi.Spool,
    need: *const c_abi.JofVerifyRequirements,
    work: *c_abi.JofVerifyWorkspace,
    reference: bool,
    info: *c_abi.JofInfo,
) c_abi.Ra8Err {
    const tile_h: u16 = if (reference) 1 else need.band_height;
    var rc = deps.jof_parse(spool.read_at, spool.ctx, info.total_size, info);
    if (rc == c_abi.k_ra8_ok and !infoMatchesPolicy(info, need, tile_h)) {
        rc = c_abi.k_ra8_err_validation_failed;
    }
    if (rc == c_abi.k_ra8_ok) {
        rc = preflightAtlas(deps, spool, info, work, reference);
    }
    return rc;
}

fn emitPhaseError(report: *const c_abi.Sink, prefix: []const u8, status: c_abi.Ra8Err) void {
    var rc = fmt_report.put(report, prefix);
    fmt_report.appendField(report, status, ")\n", &rc);
}

fn prepareAtlases(
    comptime deps: c_abi.Deps,
    ref_src: *const c_abi.Source,
    band_src: *const c_abi.Source,
    need: *const c_abi.JofVerifyRequirements,
    work: *c_abi.JofVerifyWorkspace,
    ref_spool: *c_abi.Spool,
    band_spool: *c_abi.Spool,
    report: *const c_abi.Sink,
    atlases: *Atlases,
) c_abi.Ra8Err {
    var rc = produceAtlas(deps, ref_src, need, 1, work, ref_spool, &atlases.reference);
    if (rc == c_abi.k_ra8_ok) {
        rc = acceptAtlas(deps, ref_spool, need, work, true, &atlases.reference);
    }
    if (rc != c_abi.k_ra8_ok) {
        emitPhaseError(report, "verify: reference encode failed (rc=", rc);
        return rc;
    }

    rc = produceAtlas(deps, band_src, need, need.band_height, work, band_spool, &atlases.banded);
    if (rc == c_abi.k_ra8_ok) {
        rc = acceptAtlas(deps, band_spool, need, work, false, &atlases.banded);
    }
    if (rc != c_abi.k_ra8_ok) {
        emitPhaseError(report, "verify: banded encode failed (rc=", rc);
    }
    return rc;
}

fn emitGeometryLine(
    report: *const c_abi.Sink,
    info: *const c_abi.JofInfo,
    subject: *const c_abi.JofInfo,
) void {
    var rc = fmt_report.put(report, "verify: ");
    fmt_report.appendField(report, info.width, "x", &rc);
    fmt_report.appendField(report, info.height, " bpp=", &rc);
    fmt_report.appendField(report, info.bpp, " | reference 1 tile | banded ", &rc);
    fmt_report.appendField(report, subject.tile_count, " tiles of ", &rc);
    fmt_report.appendField(report, subject.tile_h, " rows\n", &rc);
}

fn abortDump(state: *CompareState) void {
    source_io.abortTransaction(state.dump);
    state.dump_ok = false;
}

fn dumpBytes(state: *CompareState, bytes: []const u8) void {
    if (!state.dump_ok) return;
    const dump = state.dump orelse return;
    const ops = dump.ops orelse return;
    const append_fn = ops.append orelse return;
    if (append_fn(dump.ctx, bytes.ptr, bytes.len) != c_abi.k_ra8_ok) {
        abortDump(state);
    }
}

fn dumpU64(state: *CompareState, value: u64) void {
    var rev: [20]u8 = undefined;
    var count: usize = 0;
    var v = value;
    while (true) {
        rev[count] = '0' + @as(u8, @intCast(v % 10));
        count += 1;
        v /= 10;
        if (v == 0) break;
    }
    var text: [20]u8 = undefined;
    for (0..count) |i| {
        text[i] = rev[count - 1 - i];
    }
    dumpBytes(state, text[0..count]);
}

fn dumpHeader(state: *CompareState) void {
    const magic: [3]u8 = .{ 'P', if (state.need.bpp == 1) '5' else '6', '\n' };
    dumpBytes(state, &magic);
    dumpU64(state, state.need.width);
    dumpBytes(state, " ");
    dumpU64(state, state.need.height);
    dumpBytes(state, "\n255\n");
}

fn dumpRow(state: *CompareState, row: []const u8) void {
    if (state.dump == null or !state.dump_ok) return;
    if (state.need.bpp != 4) {
        dumpBytes(state, row[0..state.need.row_bytes]);
        return;
    }
    const row_buf = state.work.row orelse return;
    const width: usize = state.need.width;
    if (width * 3 > state.work.row_cap or width * 4 > row.len) {
        abortDump(state);
        return;
    }
    for (0..width) |x| {
        const dst_off = x * 3;
        const src_off = x * 4;
        row_buf[dst_off + 0] = row[src_off + 0];
        row_buf[dst_off + 1] = row[src_off + 1];
        row_buf[dst_off + 2] = row[src_off + 2];
    }
    dumpBytes(state, row_buf[0 .. width * 3]);
}

fn emitDiff(state: *CompareState, x: u64, y: u64, reference: u8, banded: u8) void {
    if (state.shown >= max_diffs) return;
    var rc = fmt_report.put(state.report, "  DIFF at pixel (");
    fmt_report.appendField(state.report, x, ", ", &rc);
    fmt_report.appendField(state.report, y, "): reference=0x", &rc);
    if (rc == c_abi.k_ra8_ok) {
        const ref_hex: [2]u8 = .{ hex_digits[reference >> 4], hex_digits[reference & 0xF] };
        rc = fmt_report.put(state.report, &ref_hex);
    }
    if (rc == c_abi.k_ra8_ok) rc = fmt_report.put(state.report, " banded=0x");
    if (rc == c_abi.k_ra8_ok) {
        const band_hex: [2]u8 = .{ hex_digits[banded >> 4], hex_digits[banded & 0xF] };
        rc = fmt_report.put(state.report, &band_hex);
    }
    if (rc == c_abi.k_ra8_ok) _ = fmt_report.put(state.report, "\n");
    state.shown += 1;
}

fn compareRow(state: *CompareState, band_row: []const u8, y: u16) void {
    const ref_row = state.work.row orelse return;
    const len: usize = state.need.row_bytes;
    for (0..len) |i| {
        if (ref_row[i] != band_row[i]) {
            state.diffs += 1;
            emitDiff(state, i / state.need.bpp, y, ref_row[i], band_row[i]);
        }
    }
    dumpRow(state, band_row);
}

fn compareRasters(comptime deps: c_abi.Deps, state: *CompareState) c_abi.Ra8Err {
    if (state.dump != null) {
        state.dump_ok = true;
        dumpHeader(state);
    }
    const band_tile = state.work.band_tile orelse return c_abi.k_ra8_err_null_ptr;
    const row_bytes: usize = state.need.row_bytes;

    var global_y: u16 = 0;
    var band_y: u16 = 0;
    while (band_y < state.ginfo.tile_rows) : (band_y += 1) {
        var band_w: u16 = 0;
        var band_h: u16 = 0;
        var rc = deps.jof_read_tile(
            state.got.read_at,
            state.got.ctx,
            state.ginfo,
            0,
            band_y,
            state.work.scratch,
            state.work.scratch_cap,
            state.work.band_tile,
            state.work.band_tile_cap,
            &band_w,
            &band_h,
        );
        if (rc != c_abi.k_ra8_ok or band_w != state.need.width) {
            return if (rc == c_abi.k_ra8_ok) c_abi.k_ra8_err_validation_failed else rc;
        }
        if (@as(u32, global_y) + band_h > state.need.height or
            @as(u64, band_h) * row_bytes > state.work.band_tile_cap)
        {
            return c_abi.k_ra8_err_validation_failed;
        }
        var row: u16 = 0;
        while (row < band_h) : (row += 1) {
            var ref_w: u16 = 0;
            var ref_h: u16 = 0;
            rc = deps.jof_read_tile(
                state.ref.read_at,
                state.ref.ctx,
                state.rinfo,
                0,
                global_y,
                state.work.scratch,
                state.work.scratch_cap,
                state.work.row,
                state.work.row_cap,
                &ref_w,
                &ref_h,
            );
            if (rc != c_abi.k_ra8_ok or ref_w != state.need.width or ref_h != 1) {
                return if (rc == c_abi.k_ra8_ok) c_abi.k_ra8_err_validation_failed else rc;
            }
            const off = @as(usize, row) * row_bytes;
            compareRow(state, band_tile[off..][0..row_bytes], global_y);
            global_y += 1;
        }
    }
    return if (global_y == state.need.height) c_abi.k_ra8_ok else c_abi.k_ra8_err_validation_failed;
}

fn finishDump(state: *CompareState, dump_name: ?[*:0]const u8) void {
    const dump = state.dump orelse return;
    if (state.dump_ok) {
        if (dump.ops.?.commit.?(dump.ctx) != c_abi.k_ra8_ok) {
            abortDump(state);
        }
    }
    var rc = fmt_report.put(state.report, "  wrote reassembled raster to ");
    if (rc == c_abi.k_ra8_ok) rc = fmt_report.putCStr(state.report, dump_name.?);
    if (rc == c_abi.k_ra8_ok) {
        _ = fmt_report.put(state.report, if (state.dump_ok) " (ok)\n" else " (FAILED)\n");
    }
}

fn emitVerdict(report: *const c_abi.Sink, diffs: u64) void {
    var rc = fmt_report.put(report, "verdict: ");
    if (rc == c_abi.k_ra8_ok) {
        const label = if (diffs == 0) "ROUND-TRIP EXACT -- the produced file is correct" else "MISMATCH";
        rc = fmt_report.put(report, label);
    }
    if (rc == c_abi.k_ra8_ok) rc = fmt_report.put(report, " (");
    fmt_report.appendField(report, diffs, " differing bytes)\n", &rc);
}

fn checkBindings(
    ref_src: ?*const c_abi.Source,
    band_src: ?*const c_abi.Source,
    need_ptr: ?*const c_abi.JofVerifyRequirements,
    work_ptr: ?*const c_abi.JofVerifyWorkspace,
    ref_spool_ptr: ?*const c_abi.Spool,
    band_spool_ptr: ?*const c_abi.Spool,
    report_ptr: ?*const c_abi.Sink,
) c_abi.Ra8Err {
    const rs = ref_src orelse return c_abi.k_ra8_err_null_ptr;
    const bs = band_src orelse return c_abi.k_ra8_err_null_ptr;
    const need = need_ptr orelse return c_abi.k_ra8_err_null_ptr;
    const work = work_ptr orelse return c_abi.k_ra8_err_null_ptr;
    const rp = ref_spool_ptr orelse return c_abi.k_ra8_err_null_ptr;
    const bp = band_spool_ptr orelse return c_abi.k_ra8_err_null_ptr;
    const rep = report_ptr orelse return c_abi.k_ra8_err_null_ptr;

    if (rs.read_at == null or
        bs.read_at == null or
        rs.ctx == bs.ctx or
        rp.read_at == null or
        rp.append == null or
        rp.seal == null or
        bp.read_at == null or
        bp.append == null or
        bp.seal == null or
        rp.ctx == bp.ctx or
        rep.write == null)
    {
        return c_abi.k_ra8_err_null_ptr;
    }

    if (need.width == 0 or
        need.height == 0 or
        need.band_height == 0 or
        need.bpp == 0 or
        need.bpp > 4)
    {
        return c_abi.k_ra8_err_invalid_size;
    }

    const producer = @max(need.reference_work_bytes, need.banded_work_bytes);
    const webp_bad = need.webp_work_bytes != 0 and
        (work.webp_work == null or work.webp_work_cap < need.webp_work_bytes);
    if (work.work == null or
        work.work_cap < producer or
        webp_bad or
        work.band_tile == null or
        work.band_tile_cap < need.band_tile_bytes or
        work.scratch == null or
        work.scratch_cap < need.scratch_bytes or
        work.row == null or
        work.row_cap < need.row_bytes)
    {
        return c_abi.k_ra8_err_invalid_size;
    }
    return c_abi.k_ra8_ok;
}

/// Runs two-spool JOF round-trip verification and optional PPM transaction staging.
pub fn jofVerifyStream(
    comptime deps: c_abi.Deps,
    ref_src: ?*const c_abi.Source,
    band_src: ?*const c_abi.Source,
    need_ptr: ?*const c_abi.JofVerifyRequirements,
    work_ptr: ?*c_abi.JofVerifyWorkspace,
    ref_spool: ?*c_abi.Spool,
    band_spool: ?*c_abi.Spool,
    dump: ?*c_abi.Transaction,
    dump_name: ?[*:0]const u8,
    report: ?*const c_abi.Sink,
) c_abi.Ra8Err {
    var rc = checkBindings(ref_src, band_src, need_ptr, work_ptr, ref_spool, band_spool, report);
    const dump_bad = if (dump) |d| blk: {
        const ops = d.ops orelse break :blk true;
        break :blk ops.append == null or ops.commit == null or ops.abort == null or dump_name == null;
    } else false;

    if (rc != c_abi.k_ra8_ok or dump_bad) {
        return if (rc == c_abi.k_ra8_ok) c_abi.k_ra8_err_null_ptr else rc;
    }

    const rs = ref_src.?;
    const bs = band_src.?;
    const need = need_ptr.?;
    const work = work_ptr.?;
    const rsp = ref_spool.?;
    const bsp = band_spool.?;
    const rep = report.?;

    var atlases = std.mem.zeroes(Atlases);
    rc = prepareAtlases(deps, rs, bs, need, work, rsp, bsp, rep, &atlases);
    if (rc != c_abi.k_ra8_ok) {
        source_io.abortTransaction(dump);
        return rc;
    }

    emitGeometryLine(rep, &atlases.reference, &atlases.banded);
    var compare = CompareState{
        .need = need,
        .work = work,
        .ref = rsp,
        .got = bsp,
        .rinfo = &atlases.reference,
        .ginfo = &atlases.banded,
        .dump = dump,
        .report = rep,
    };

    rc = compareRasters(deps, &compare);
    if (rc == c_abi.k_ra8_ok) rc = source_io.checkStable(rs);
    if (rc == c_abi.k_ra8_ok) rc = source_io.checkStable(bs);
    if (rc != c_abi.k_ra8_ok) {
        abortDump(&compare);
        return rc;
    }

    finishDump(&compare, dump_name);
    emitVerdict(rep, compare.diffs);
    return if (compare.diffs == 0) c_abi.k_ra8_ok else c_abi.k_ra8_err_validation_failed;
}
