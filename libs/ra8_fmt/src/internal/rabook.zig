//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Strict callback-driven RBKC container and RABOOK1 payload inspection.

const std = @import("std");
const c_abi = @import("c_abi");
const fmt_report = @import("fmt_report");
const source_io = @import("source_io");

const max_rows: u32 = 4096;
const idx_width: u32 = 5;
const num_width: u32 = 10;

const SourceAdapter = struct {
    source: *const c_abi.Source,
};

fn exactReadCallback(
    opaque_ctx: ?*anyopaque,
    offset: u64,
    bytes: ?[*]u8,
    len: u32,
) callconv(.c) c_abi.Ra8Err {
    const adapter: *SourceAdapter = @ptrCast(@alignCast(opaque_ctx orelse return c_abi.k_ra8_err_null_ptr));
    if (len != 0 and bytes == null) return c_abi.k_ra8_err_null_ptr;
    const slice: []u8 = if (len == 0) &[_]u8{} else bytes.?[0..len];
    return source_io.readExactLoop(adapter.source, offset, slice, c_abi.k_ra8_err_invalid_size);
}

fn makeInflateCallback(comptime deps: c_abi.Deps) c_abi.BookInflateFn {
    return struct {
        fn inflate(
            src: ?*const anyopaque,
            src_len: usize,
            dst: ?*anyopaque,
            dst_cap: usize,
            out_len: ?*usize,
        ) callconv(.c) c_abi.Ra8Err {
            const out_ptr = out_len orelse return c_abi.k_ra8_err_null_ptr;
            if (src == null or dst == null) return c_abi.k_ra8_err_null_ptr;
            const flags: c_int = c_abi.TINFL_FLAG_PARSE_ZLIB_HEADER | c_abi.TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF;
            const result = deps.tinfl_decompress_mem_to_mem(dst, dst_cap, src, src_len, flags);
            if (result == c_abi.TINFL_DECOMPRESS_MEM_TO_MEM_FAILED) {
                return c_abi.k_ra8_err_validation_failed;
            }
            out_ptr.* = result;
            return c_abi.k_ra8_ok;
        }
    }.inflate;
}

fn emitHeader(
    report: *const c_abi.Sink,
    source: *const c_abi.Source,
    reader: *const c_abi.BookChunked,
) c_abi.Ra8Err {
    var rc = fmt_report.put(report, "RBKC rabook container: ");
    if (rc == c_abi.k_ra8_ok) rc = fmt_report.putU64Padded(report, source.size, 0);
    if (rc == c_abi.k_ra8_ok) rc = fmt_report.put(report, " bytes\n");
    if (rc == c_abi.k_ra8_ok) rc = fmt_report.putValueLine(report, "  chunk_bytes    : ", reader.chunk_bytes, "\n");
    if (rc == c_abi.k_ra8_ok) rc = fmt_report.putValueLine(report, "  inflated_total : ", reader.inflated_total, "\n");
    if (rc == c_abi.k_ra8_ok) rc = fmt_report.putValueLine(report, "  chunk_count    : ", reader.chunk_count, "\n");
    if (rc == c_abi.k_ra8_ok) rc = fmt_report.putValueLine(report, "  reserved       : ", 0, "\n");
    return rc;
}

fn emitRow(
    report: *const c_abi.Sink,
    idx: u32,
    begin: u64,
    end: u64,
) c_abi.Ra8Err {
    var rc = fmt_report.put(report, "  ");
    if (rc == c_abi.k_ra8_ok) rc = fmt_report.putU64Padded(report, idx, idx_width);
    if (rc == c_abi.k_ra8_ok) rc = fmt_report.putU64Padded(report, begin, num_width + 1);
    if (rc == c_abi.k_ra8_ok) rc = fmt_report.putU64Padded(report, end, num_width + 1);
    if (rc == c_abi.k_ra8_ok) rc = fmt_report.putU64Padded(report, end - begin, num_width + 1);
    if (rc == c_abi.k_ra8_ok) rc = fmt_report.put(report, "\n");
    return rc;
}

fn emitTable(
    report: *const c_abi.Sink,
    reader: *const c_abi.BookChunked,
    verbose: bool,
) c_abi.Ra8Err {
    if (!verbose) return c_abi.k_ra8_ok;
    var rc = fmt_report.put(report, "  entry     start        end       length\n");
    const table = reader.table orelse return c_abi.k_ra8_err_invalid_state;
    const rows: u32 = if (reader.chunk_count > max_rows) max_rows else reader.chunk_count;
    var i: u32 = 0;
    while (i < rows and rc == c_abi.k_ra8_ok) : (i += 1) {
        const begin = reader.payload_off + table[i];
        const end = reader.payload_off + table[i + 1];
        rc = emitRow(report, i, begin, end);
    }
    return rc;
}

/// Validates and reports an RBKC chunked container and its inner RABOOK1 payload.
pub fn rabookInspectStream(
    comptime deps: c_abi.Deps,
    source_ptr: ?*const c_abi.Source,
    verbose: bool,
    workspace_ptr: ?*c_abi.RabookInspectWorkspace,
    report_ptr: ?*const c_abi.Sink,
) c_abi.Ra8Err {
    const source = source_ptr orelse return c_abi.k_ra8_err_null_ptr;
    const workspace = workspace_ptr orelse return c_abi.k_ra8_err_null_ptr;
    const report = report_ptr orelse return c_abi.k_ra8_err_null_ptr;
    if (source.read_at == null or
        workspace.table == null or
        workspace.compressed == null or
        workspace.chunk == null or
        workspace.scratch == null or
        report.write == null)
    {
        return c_abi.k_ra8_err_null_ptr;
    }

    var adapter = SourceAdapter{ .source = source };
    var reader = std.mem.zeroes(c_abi.BookChunked);
    var rc = deps.book_chunked_open(
        &reader,
        exactReadCallback,
        &adapter,
        source.size,
        makeInflateCallback(deps),
        workspace.table,
        workspace.table_cap,
        workspace.compressed,
        workspace.compressed_cap,
    );
    var header = std.mem.zeroes(c_abi.BookHeader);
    if (rc == c_abi.k_ra8_ok) {
        rc = deps.book_chunked_validate_strict(
            &reader,
            workspace.chunk,
            workspace.chunk_cap,
            workspace.scratch,
            workspace.scratch_cap,
            &header,
        );
    }
    if (rc == c_abi.k_ra8_ok) {
        rc = source_io.checkStable(source);
    }
    if (rc != c_abi.k_ra8_ok) {
        _ = fmt_report.put(report, "verdict: INVALID (strict RBKC/RABOOK1 validation failed)\n");
        return rc;
    }
    rc = emitHeader(report, source, &reader);
    if (rc == c_abi.k_ra8_ok) {
        rc = emitTable(report, &reader, verbose);
    }
    if (rc == c_abi.k_ra8_ok) {
        rc = fmt_report.put(report, "verdict: VALID (chunk table monotonic and complete)\n");
    }
    return rc;
}
