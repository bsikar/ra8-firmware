//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Callback-driven image probing and streaming JOF atlas conversion.

const std = @import("std");
const c_abi = @import("c_abi");
const fmt_report = @import("fmt_report");
const source_io = @import("source_io");

const band_height: u16 = 256;
const probe_bytes: usize = 64;
const probe_min: usize = 12;

const jpeg_marker: u8 = 0xFF;
const jpeg_soi: u8 = 0xD8;
const jpeg_eoi: u8 = 0xD9;
const jpeg_sos: u8 = 0xDA;
const jpeg_sof0: u8 = 0xC0;
const jpeg_rst_lo: u8 = 0xD0;
const jpeg_rst_hi: u8 = 0xD7;
const jpeg_tem: u8 = 0x01;
const jpeg_8bit: u8 = 8;

const png_dims_end: usize = 24;
const png_w_off: usize = 16;
const png_h_off: usize = 20;

const SourceKind = enum {
    jpeg,
    png,
    webp,
};

fn readExactChecked(
    source: *const c_abi.Source,
    offset: u64,
    buf: []u8,
    raw_err: *c_abi.Ra8Err,
) bool {
    const rc = source_io.readExactLoop(source, offset, buf, c_abi.k_ra8_err_protocol_error);
    if (rc != c_abi.k_ra8_ok) {
        raw_err.* = rc;
        return false;
    }
    return true;
}

fn readJpegSof0(
    source: *const c_abi.Source,
    payload: u64,
    segment_len: u16,
    out_w: *u16,
    out_h: *u16,
) c_abi.Ra8Err {
    if (segment_len < 8) return c_abi.k_ra8_err_protocol_error;
    var fields: [5]u8 = undefined;
    const rc = source_io.readExactLoop(source, payload, &fields, c_abi.k_ra8_err_protocol_error);
    if (rc != c_abi.k_ra8_ok) return rc;

    const h = source_io.be16(fields[1..3]);
    const w = source_io.be16(fields[3..5]);
    if (fields[0] != jpeg_8bit) return c_abi.k_ra8_err_not_supported;
    if (w == 0 or h == 0 or w > c_abi.k_jof_max_dim or h > c_abi.k_jof_max_dim) {
        return c_abi.k_ra8_err_invalid_size;
    }
    out_w.* = w;
    out_h.* = h;
    return c_abi.k_ra8_ok;
}

fn probeJpegDims(source: *const c_abi.Source, out_w: *u16, out_h: *u16) c_abi.Ra8Err {
    var offset: u64 = 2;
    var raw_err: c_abi.Ra8Err = c_abi.k_ra8_ok;
    while (offset < source.size) {
        var byte: [1]u8 = .{0};
        while (true) {
            if (!readExactChecked(source, offset, &byte, &raw_err)) return raw_err;
            offset += 1;
            if (byte[0] == jpeg_marker) break;
        }
        while (true) {
            if (!readExactChecked(source, offset, &byte, &raw_err)) return raw_err;
            offset += 1;
            if (byte[0] != jpeg_marker) break;
        }
        const marker = byte[0];
        if (marker == 0) return c_abi.k_ra8_err_protocol_error;

        const standalone = (marker == jpeg_soi) or (marker == jpeg_tem) or (marker >= jpeg_rst_lo and marker <= jpeg_rst_hi);
        if (standalone) continue;
        if (marker == jpeg_eoi or marker == jpeg_sos) return c_abi.k_ra8_err_protocol_error;

        var len_bytes: [2]u8 = undefined;
        if (!readExactChecked(source, offset, &len_bytes, &raw_err)) return raw_err;
        const segment_len = source_io.be16(&len_bytes);
        if (segment_len < 2) return c_abi.k_ra8_err_protocol_error;

        if (source_io.isJpegSofMarker(marker)) {
            if (marker != jpeg_sof0) return c_abi.k_ra8_err_not_supported;
            const payload = std.math.add(u64, offset, 2) catch return c_abi.k_ra8_err_protocol_error;
            return readJpegSof0(source, payload, segment_len, out_w, out_h);
        }
        offset = std.math.add(u64, offset, segment_len) catch return c_abi.k_ra8_err_protocol_error;
        if (offset > source.size) return c_abi.k_ra8_err_protocol_error;
    }
    return c_abi.k_ra8_err_protocol_error;
}

fn probePngDims(prefix: *const [png_dims_end]u8, out_w: *u16, out_h: *u16) c_abi.Ra8Err {
    const w = source_io.be32(prefix[png_w_off..][0..4]);
    const h = source_io.be32(prefix[png_h_off..][0..4]);
    if (w == 0 or h == 0 or w > c_abi.k_jof_max_dim or h > c_abi.k_jof_max_dim) {
        return c_abi.k_ra8_err_invalid_size;
    }
    out_w.* = @intCast(w);
    out_h.* = @intCast(h);
    return c_abi.k_ra8_ok;
}

fn mapKind(format: u32) ?SourceKind {
    return switch (format) {
        c_abi.k_imgdec_format_jpeg => .jpeg,
        c_abi.k_imgdec_format_png => .png,
        c_abi.k_imgdec_format_webp => .webp,
        else => null,
    };
}

fn probeSource(
    comptime deps: c_abi.Deps,
    source: *const c_abi.Source,
    out_kind: *SourceKind,
    out_w: *u16,
    out_h: *u16,
) c_abi.Ra8Err {
    comptime {
        if (probe_min != c_abi.k_imgdec_sniff_bytes) {
            @compileError("probe_min must match k_imgdec_sniff_bytes");
        }
    }
    var prefix: [probe_bytes]u8 = undefined;
    const count: usize = if (source.size < probe_bytes) @intCast(source.size) else probe_bytes;
    if (count < probe_min) return c_abi.k_ra8_err_not_supported;

    var rc = source_io.readExactLoop(source, 0, prefix[0..count], c_abi.k_ra8_err_protocol_error);
    if (rc != c_abi.k_ra8_ok) return rc;

    var format: u32 = c_abi.k_imgdec_format_none;
    rc = deps.ra8_imgdec_sniff(&prefix, @intCast(count), &format);
    if (rc != c_abi.k_ra8_ok) return c_abi.k_ra8_err_not_supported;

    const kind = mapKind(format) orelse return c_abi.k_ra8_err_not_supported;
    switch (kind) {
        .jpeg => {
            out_kind.* = .jpeg;
            return probeJpegDims(source, out_w, out_h);
        },
        .png => {
            if (count < png_dims_end) return c_abi.k_ra8_err_not_supported;
            out_kind.* = .png;
            return probePngDims(prefix[0..png_dims_end], out_w, out_h);
        },
        .webp => {
            var w: u32 = 0;
            var h: u32 = 0;
            rc = deps.ra8_webp_get_info(&prefix, count, &w, &h);
            if (rc != c_abi.k_ra8_ok) return rc;
            if (w == 0 or h == 0 or w > c_abi.k_jof_max_dim or h > c_abi.k_jof_max_dim) {
                return c_abi.k_ra8_err_invalid_size;
            }
            out_kind.* = .webp;
            out_w.* = @intCast(w);
            out_h.* = @intCast(h);
            return c_abi.k_ra8_ok;
        },
    }
}

fn appendSinkCallback(
    ctx: ?*anyopaque,
    bytes: ?[*]const u8,
    len: usize,
) callconv(.c) c_abi.Ra8Err {
    const tx: *c_abi.Transaction = @ptrCast(@alignCast(ctx orelse return c_abi.k_ra8_err_null_ptr));
    const ops = tx.ops orelse return c_abi.k_ra8_err_null_ptr;
    const append_fn = ops.append orelse return c_abi.k_ra8_err_null_ptr;
    return append_fn(tx.ctx, bytes, len);
}

fn emitReport(
    report: *const c_abi.Sink,
    info: *const c_abi.JofInfo,
    output_name: [*:0]const u8,
) c_abi.Ra8Err {
    var rc = fmt_report.put(report, "convert: ");
    fmt_report.appendField(report, info.width, "x", &rc);
    fmt_report.appendField(report, info.height, " bpp=", &rc);
    fmt_report.appendField(report, info.bpp, " band=", &rc);
    fmt_report.appendField(report, info.tile_h, " tiles=", &rc);
    fmt_report.appendField(report, info.tile_count, " -> ", &rc);
    if (rc == c_abi.k_ra8_ok) rc = fmt_report.putCStr(report, output_name);
    if (rc == c_abi.k_ra8_ok) rc = fmt_report.put(report, " (");
    fmt_report.appendField(report, info.total_size, " bytes)\n", &rc);
    return rc;
}

/// Probes source dimensions and fills exact producer workspace requirements.
pub fn jofConvertRequirements(
    comptime deps: c_abi.Deps,
    source_ptr: ?*const c_abi.Source,
    out_ptr: ?*c_abi.JofConvertRequirements,
) c_abi.Ra8Err {
    const source = source_ptr orelse return c_abi.k_ra8_err_null_ptr;
    if (source.read_at == null) return c_abi.k_ra8_err_null_ptr;
    const out = out_ptr orelse return c_abi.k_ra8_err_null_ptr;

    out.* = std.mem.zeroes(c_abi.JofConvertRequirements);
    var kind: SourceKind = .jpeg;
    var width: u16 = 0;
    var height: u16 = 0;
    const rc = probeSource(deps, source, &kind, &width, &height);
    if (rc != c_abi.k_ra8_ok) return rc;

    const tile_w = width;
    const tile_h = if (height < band_height) height else band_height;
    const work_bytes = deps.jof_work_bytes(width, height, tile_w, tile_h);
    if (work_bytes == 0) return c_abi.k_ra8_err_invalid_size;

    var webp_bytes: u32 = 0;
    if (kind == .webp) {
        if (source.size > std.math.maxInt(u32)) return c_abi.k_ra8_err_invalid_size;
        webp_bytes = deps.jof_webp_work_bytes(width, height, @intCast(source.size));
        if (webp_bytes == 0) return c_abi.k_ra8_err_invalid_size;
    }

    out.width = width;
    out.height = height;
    out.tile_width = tile_w;
    out.tile_height = tile_h;
    out.work_bytes = work_bytes;
    out.webp_work_bytes = webp_bytes;
    return c_abi.k_ra8_ok;
}

/// Streams a source image into a durably published JOF atlas and emits a report.
pub fn jofConvertStream(
    comptime deps: c_abi.Deps,
    source_ptr: ?*const c_abi.Source,
    req_ptr: ?*const c_abi.JofConvertRequirements,
    ws_ptr: ?*const c_abi.JofConvertWorkspace,
    tx_ptr: ?*c_abi.Transaction,
    report_ptr: ?*const c_abi.Sink,
    output_name_ptr: ?[*:0]const u8,
) c_abi.Ra8Err {
    const tx = tx_ptr orelse return c_abi.k_ra8_err_null_ptr;
    const source = source_ptr orelse {
        source_io.abortTransaction(tx);
        return c_abi.k_ra8_err_null_ptr;
    };
    const req = req_ptr orelse {
        source_io.abortTransaction(tx);
        return c_abi.k_ra8_err_null_ptr;
    };
    const ws = ws_ptr orelse {
        source_io.abortTransaction(tx);
        return c_abi.k_ra8_err_null_ptr;
    };
    const ops = tx.ops orelse {
        source_io.abortTransaction(tx);
        return c_abi.k_ra8_err_null_ptr;
    };
    const report = report_ptr orelse {
        source_io.abortTransaction(tx);
        return c_abi.k_ra8_err_null_ptr;
    };
    const output_name = output_name_ptr orelse {
        source_io.abortTransaction(tx);
        return c_abi.k_ra8_err_null_ptr;
    };
    if (source.read_at == null or
        ops.append == null or
        ops.commit == null or
        ops.abort == null or
        report.write == null)
    {
        source_io.abortTransaction(tx);
        return c_abi.k_ra8_err_null_ptr;
    }

    const webp_missing = req.webp_work_bytes != 0 and
        (ws.webp_work == null or ws.webp_work_cap < req.webp_work_bytes);
    if (ws.work == null or ws.work_cap < req.work_bytes or webp_missing) {
        source_io.abortTransaction(tx);
        return c_abi.k_ra8_err_invalid_size;
    }

    var pull = source_io.PullCursor{ .source = source, .offset = 0 };
    const cfg = c_abi.JofProduceCfg{
        .pull = source_io.pullCallback,
        .pull_ctx = &pull,
        .sink = appendSinkCallback,
        .sink_ctx = tx,
        .tile_w = req.tile_width,
        .tile_h = req.tile_height,
        .codec = c_abi.k_jof_codec_deflate,
        .max_width = req.width,
        .max_height = req.height,
        .work = ws.work,
        .work_cap = ws.work_cap,
        .webp_work = ws.webp_work,
        .webp_work_cap = ws.webp_work_cap,
    };

    var info = std.mem.zeroes(c_abi.JofInfo);
    var rc = deps.jof_produce(&cfg, &info);
    if (rc != c_abi.k_ra8_ok) {
        source_io.abortTransaction(tx);
        return rc;
    }
    rc = ops.commit.?(tx.ctx);
    if (rc != c_abi.k_ra8_ok) {
        source_io.abortTransaction(tx);
        return rc;
    }
    return emitReport(report, &info, output_name);
}
