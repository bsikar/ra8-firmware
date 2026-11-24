//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Callback-driven JOF atlas structural inspection and bounded reporting.

const c_abi = @import("c_abi");
const fmt_report = @import("fmt_report");
const source_io = @import("source_io");

const max_rows: u32 = 4096;
const hex_row_bytes: usize = 16;
const col_width: u32 = 10;
const ascii_low: u8 = 32;
const ascii_high: u8 = 126;

fn emitHexDumpHeader(
    report: *const c_abi.Sink,
    label: []const u8,
    offset: u64,
    len: usize,
) c_abi.Ra8Err {
    var rc = fmt_report.put(report, label);
    if (rc == c_abi.k_ra8_ok) rc = fmt_report.put(report, " (");
    if (rc == c_abi.k_ra8_ok) rc = fmt_report.putU64Padded(report, len, 0);
    if (rc == c_abi.k_ra8_ok) rc = fmt_report.put(report, " bytes at +");
    if (rc == c_abi.k_ra8_ok) rc = fmt_report.putU64Padded(report, offset, 0);
    if (rc == c_abi.k_ra8_ok) rc = fmt_report.put(report, "):\n");
    return rc;
}

fn emitHexDumpRow(
    report: *const c_abi.Sink,
    offset: u64,
    row: []const u8,
) c_abi.Ra8Err {
    var rc = fmt_report.put(report, "  ");
    if (rc == c_abi.k_ra8_ok) rc = fmt_report.putHex(report, offset, 8);
    if (rc == c_abi.k_ra8_ok) rc = fmt_report.put(report, "  ");

    var i: usize = 0;
    while (i < hex_row_bytes and rc == c_abi.k_ra8_ok) : (i += 1) {
        if (i < row.len) {
            rc = fmt_report.putHex(report, row[i], 2);
            if (rc == c_abi.k_ra8_ok) rc = fmt_report.putChar(report, ' ');
        } else {
            rc = fmt_report.put(report, "   ");
        }
    }

    if (rc == c_abi.k_ra8_ok) rc = fmt_report.put(report, " |");
    i = 0;
    while (i < row.len and rc == c_abi.k_ra8_ok) : (i += 1) {
        const printable = row[i] >= ascii_low and row[i] <= ascii_high;
        rc = fmt_report.putChar(report, if (printable) row[i] else '.');
    }
    if (rc == c_abi.k_ra8_ok) rc = fmt_report.put(report, "|\n");
    return rc;
}

fn emitHexDump(
    source: *const c_abi.Source,
    report: *const c_abi.Sink,
    label: []const u8,
    offset: u64,
    len: usize,
) c_abi.Ra8Err {
    var rc = emitHexDumpHeader(report, label, offset, len);
    var row: [hex_row_bytes]u8 = undefined;
    var done: usize = 0;
    while (done < len and rc == c_abi.k_ra8_ok) {
        const remain = len - done;
        const count: usize = if (remain > hex_row_bytes) hex_row_bytes else remain;
        rc = source_io.readExactSingle(source, offset + done, row[0..count]);
        if (rc == c_abi.k_ra8_ok) {
            rc = emitHexDumpRow(report, offset + done, row[0..count]);
        }
        done += count;
    }
    return rc;
}

fn emitDimensions(info: *const c_abi.JofInfo, report: *const c_abi.Sink) c_abi.Ra8Err {
    var rc = fmt_report.put(report, "  image      : ");
    if (rc == c_abi.k_ra8_ok) rc = fmt_report.putU64Padded(report, info.width, 0);
    if (rc == c_abi.k_ra8_ok) rc = fmt_report.put(report, " x ");
    if (rc == c_abi.k_ra8_ok) rc = fmt_report.putU64Padded(report, info.height, 0);
    if (rc == c_abi.k_ra8_ok) rc = fmt_report.put(report, " px\n  tile       : ");
    if (rc == c_abi.k_ra8_ok) rc = fmt_report.putU64Padded(report, info.tile_w, 0);
    if (rc == c_abi.k_ra8_ok) rc = fmt_report.put(report, " x ");
    if (rc == c_abi.k_ra8_ok) rc = fmt_report.putU64Padded(report, info.tile_h, 0);
    if (rc == c_abi.k_ra8_ok) rc = fmt_report.put(report, " px\n  grid       : ");
    if (rc == c_abi.k_ra8_ok) rc = fmt_report.putU64Padded(report, info.tile_cols, 0);
    if (rc == c_abi.k_ra8_ok) rc = fmt_report.put(report, " cols x ");
    if (rc == c_abi.k_ra8_ok) rc = fmt_report.putU64Padded(report, info.tile_rows, 0);
    if (rc == c_abi.k_ra8_ok) rc = fmt_report.put(report, " rows\n");
    return rc;
}

fn emitGeometry(
    source: *const c_abi.Source,
    info: *const c_abi.JofInfo,
    report: *const c_abi.Sink,
) c_abi.Ra8Err {
    var rc = fmt_report.put(report, "JOF atlas: ");
    if (rc == c_abi.k_ra8_ok) rc = fmt_report.putU64Padded(report, source.size, 0);
    if (rc == c_abi.k_ra8_ok) rc = fmt_report.put(report, " bytes\n");
    if (rc == c_abi.k_ra8_ok) rc = emitDimensions(info, report);
    if (rc == c_abi.k_ra8_ok) rc = fmt_report.putValueLine(report, "  bpp        : ", info.bpp, "\n");
    if (rc == c_abi.k_ra8_ok) rc = fmt_report.putValueLine(report, "  codec      : ", info.codec, " (");
    if (rc == c_abi.k_ra8_ok) {
        const codec_text = if (info.codec == c_abi.k_jof_codec_deflate) "deflate)\n" else "raw)\n";
        rc = fmt_report.put(report, codec_text);
    }
    if (rc == c_abi.k_ra8_ok) rc = fmt_report.putValueLine(report, "  tile_count : ", info.tile_count, "\n");
    if (rc == c_abi.k_ra8_ok) rc = fmt_report.putValueLine(report, "  index_off  : ", info.index_off, "\n");
    if (rc == c_abi.k_ra8_ok) {
        rc = fmt_report.putValueLine(report, "  total_size : ", info.total_size, "\n  longstrip  : ");
    }
    if (rc == c_abi.k_ra8_ok) {
        const ls_text = if (info.tile_w == info.width) "YES (tile_w == width)\n" else "NO (tile_w != width)\n";
        rc = fmt_report.put(report, ls_text);
    }
    return rc;
}

fn emitTableRow(
    report: *const c_abi.Sink,
    index: u32,
    record: *const c_abi.JofAuditRecord,
) c_abi.Ra8Err {
    var rc = fmt_report.putU64Padded(report, index, 8);
    if (rc == c_abi.k_ra8_ok) rc = fmt_report.putU64Padded(report, record.offset, col_width);
    if (rc == c_abi.k_ra8_ok) rc = fmt_report.putU64Padded(report, record.length, col_width);
    if (rc == c_abi.k_ra8_ok) rc = fmt_report.putU64Padded(report, record.payload, col_width);
    if (rc == c_abi.k_ra8_ok) rc = fmt_report.putU64Padded(report, record.width, 6);
    if (rc == c_abi.k_ra8_ok) rc = fmt_report.putU64Padded(report, record.height, 6);
    if (rc == c_abi.k_ra8_ok) rc = fmt_report.put(report, "   ");
    if (rc == c_abi.k_ra8_ok) rc = fmt_report.putHex(report, record.content_hash, 8);
    if (rc == c_abi.k_ra8_ok) rc = fmt_report.putChar(report, '\n');
    return rc;
}

fn emitTable(
    records_ptr: ?[*]c_abi.JofAuditRecord,
    count: u32,
    report: *const c_abi.Sink,
) c_abi.Ra8Err {
    var rc = fmt_report.put(report, "  tile      offset    length   payload   tw    th   hash\n");
    const shown: u32 = if (count > max_rows) max_rows else count;
    if (shown > 0 and records_ptr == null) return c_abi.k_ra8_err_null_ptr;
    var i: u32 = 0;
    while (i < shown and rc == c_abi.k_ra8_ok) : (i += 1) {
        rc = emitTableRow(report, i, &records_ptr.?[i]);
    }
    if (shown < count and rc == c_abi.k_ra8_ok) rc = fmt_report.put(report, "  ... ");
    if (shown < count and rc == c_abi.k_ra8_ok) rc = fmt_report.putU64Padded(report, count - shown, 0);
    if (shown < count and rc == c_abi.k_ra8_ok) rc = fmt_report.put(report, " more tiles elided\n");
    return rc;
}

/// Runs JOF structural audit via `deps.jof_audit` and formats the stream report.
pub fn jofInspectStream(
    comptime deps: c_abi.Deps,
    source_ptr: ?*const c_abi.Source,
    verbose: bool,
    workspace_ptr: ?*const c_abi.JofInspectWorkspace,
    report_ptr: ?*const c_abi.Sink,
) c_abi.Ra8Err {
    const source = source_ptr orelse return c_abi.k_ra8_err_null_ptr;
    if (source.read_at == null) return c_abi.k_ra8_err_null_ptr;
    const workspace = workspace_ptr orelse return c_abi.k_ra8_err_null_ptr;
    const report = report_ptr orelse return c_abi.k_ra8_err_null_ptr;
    if (report.write == null) return c_abi.k_ra8_err_null_ptr;

    var audit_ws = c_abi.JofAuditWorkspace{
        .records = workspace.records,
        .record_cap = workspace.record_cap,
        .tile = workspace.tile,
        .tile_cap = workspace.tile_cap,
        .scratch = workspace.scratch,
        .scratch_cap = workspace.scratch_cap,
    };
    var result: c_abi.JofAuditResult = .{
        .info = .{
            .width = 0,
            .height = 0,
            .tile_w = 0,
            .tile_h = 0,
            .tile_cols = 0,
            .tile_rows = 0,
            .bpp = 0,
            .codec = 0,
            .tile_count = 0,
            .index_off = 0,
            .total_size = 0,
        },
        .decoded_tiles = 0,
        .coverage_errors = 0,
        .geometry_errors = 0,
        .duplicate_candidates = 0,
    };

    const audit_rc = deps.jof_audit(source.read_at, source.ctx, source.size, &audit_ws, &result);
    if (audit_rc != c_abi.k_ra8_ok and audit_rc != c_abi.k_ra8_err_validation_failed) {
        return audit_rc;
    }

    var rc = emitGeometry(source, &result.info, report);
    if (verbose and rc == c_abi.k_ra8_ok) {
        if (source.size < c_abi.k_jof_hdr_bytes) return c_abi.k_ra8_err_invalid_size;
        rc = emitHexDump(source, report, "header", 0, c_abi.k_jof_hdr_bytes);
    }
    if (verbose and rc == c_abi.k_ra8_ok) {
        if (source.size < c_abi.k_jof_footer_bytes) return c_abi.k_ra8_err_invalid_size;
        rc = emitHexDump(source, report, "footer", source.size - c_abi.k_jof_footer_bytes, c_abi.k_jof_footer_bytes);
    }
    if (verbose and rc == c_abi.k_ra8_ok) {
        rc = emitTable(workspace.records, result.info.tile_count, report);
    }
    if (result.duplicate_candidates != 0 and rc == c_abi.k_ra8_ok) {
        rc = fmt_report.putValueLine(
            report,
            "  duplicate hash candidates: ",
            result.duplicate_candidates,
            " (diagnostic only)\n",
        );
    }
    if (rc == c_abi.k_ra8_ok) {
        const verdict = if (audit_rc == c_abi.k_ra8_ok and result.duplicate_candidates == 0)
            "verdict: VALID (coverage exact, no duplicate tiles)\n"
        else if (audit_rc == c_abi.k_ra8_ok)
            "verdict: VALID (coverage exact; duplicate hashes are diagnostic)\n"
        else
            "verdict: INVALID (see anomalies above)\n";
        rc = fmt_report.put(report, verdict);
    }
    return if (rc == c_abi.k_ra8_ok) audit_rc else rc;
}
