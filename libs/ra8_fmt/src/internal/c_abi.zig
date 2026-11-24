//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI types, constants, layout assertions, and extern dependency bindings for
//! `libs/ra8_fmt`. Declares exact extern structs matching `ra8_fmt_io.h`,
//! `ra8_fmt_stream.h`, `jof.h`, `jof_produce.h`, `jof_audit.h`, `book.h`, and
//! `book_chunked.h` without pulling C headers into portable AST checks.

pub const Ra8Err = u16;

pub const k_ra8_ok: Ra8Err = 0;
pub const k_ra8_err_no_mem: Ra8Err = 0x102;
pub const k_ra8_err_invalid_arg: Ra8Err = 0x103;
pub const k_ra8_err_invalid_state: Ra8Err = 0x104;
pub const k_ra8_err_invalid_size: Ra8Err = 0x105;
pub const k_ra8_err_not_supported: Ra8Err = 0x107;
pub const k_ra8_err_out_of_range: Ra8Err = 0x208;
pub const k_ra8_err_crc_mismatch: Ra8Err = 0x405;
pub const k_ra8_err_protocol_error: Ra8Err = 0x406;
pub const k_ra8_err_conflict: Ra8Err = 0x408;
pub const k_ra8_err_validation_failed: Ra8Err = 0x501;
pub const k_ra8_err_null_ptr: Ra8Err = 0x504;

pub const k_jof_hdr_bytes: usize = 32;
pub const k_jof_footer_bytes: usize = 16;
pub const k_jof_max_dim: u32 = 32768;
pub const k_jof_codec_raw: u8 = 0;
pub const k_jof_codec_deflate: u8 = 1;

pub const k_imgdec_format_none: u32 = 0;
pub const k_imgdec_format_jpeg: u32 = 1 << 0;
pub const k_imgdec_format_png: u32 = 1 << 1;
pub const k_imgdec_format_webp: u32 = 1 << 2;
pub const k_imgdec_sniff_bytes: usize = 12;

pub const TINFL_FLAG_PARSE_ZLIB_HEADER: c_int = 1;
pub const TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF: c_int = 4;
pub const TINFL_DECOMPRESS_MEM_TO_MEM_FAILED: usize = @import("std").math.maxInt(usize);

pub const PreadFn = *const fn (
    ctx: ?*anyopaque,
    offset: u64,
    buf: ?[*]u8,
    len: usize,
    got: ?*usize,
) callconv(.c) Ra8Err;

pub const SourceValidateFn = *const fn (
    ctx: ?*anyopaque,
    expected_size: u64,
) callconv(.c) Ra8Err;

pub const SinkWriteFn = *const fn (
    ctx: ?*anyopaque,
    bytes: ?[*]const u8,
    len: usize,
) callconv(.c) Ra8Err;

pub const SpoolSealFn = *const fn (
    ctx: ?*anyopaque,
    expected_size: u64,
) callconv(.c) Ra8Err;

pub const TransactionCommitFn = *const fn (ctx: ?*anyopaque) callconv(.c) Ra8Err;
pub const TransactionAbortFn = *const fn (ctx: ?*anyopaque) callconv(.c) void;

pub const VsourceReadFn = *const fn (
    ctx: ?*anyopaque,
    offset: u64,
    buf: ?[*]u8,
    len: u32,
) callconv(.c) Ra8Err;

pub const BookInflateFn = *const fn (
    src: ?*const anyopaque,
    src_len: usize,
    dst: ?*anyopaque,
    dst_cap: usize,
    out_len: ?*usize,
) callconv(.c) Ra8Err;

pub const JofPullFn = *const fn (
    ctx: ?*anyopaque,
    buf: ?[*]u8,
    cap: usize,
    got: ?*usize,
) callconv(.c) Ra8Err;

pub const JofSinkFn = *const fn (
    ctx: ?*anyopaque,
    buf: ?[*]const u8,
    len: usize,
) callconv(.c) Ra8Err;

pub const Source = extern struct {
    read_at: ?PreadFn,
    validate: ?SourceValidateFn,
    ctx: ?*anyopaque,
    size: u64,
};

pub const Sink = extern struct {
    write: ?SinkWriteFn,
    ctx: ?*anyopaque,
};

pub const Spool = extern struct {
    read_at: ?PreadFn,
    append: ?SinkWriteFn,
    seal: ?SpoolSealFn,
    ctx: ?*anyopaque,
};

pub const TransactionOps = extern struct {
    append: ?SinkWriteFn,
    commit: ?TransactionCommitFn,
    abort: ?TransactionAbortFn,
};

pub const Transaction = extern struct {
    ops: ?*const TransactionOps,
    ctx: ?*anyopaque,
};

pub const JofInfo = extern struct {
    width: u16,
    height: u16,
    tile_w: u16,
    tile_h: u16,
    tile_cols: u16,
    tile_rows: u16,
    bpp: u8,
    codec: u8,
    tile_count: u32,
    index_off: u32,
    total_size: u32,
};

pub const JofAuditRecord = extern struct {
    offset: u32,
    length: u32,
    payload: u32,
    content_hash: u32,
    width: u16,
    height: u16,
    uniform: bool,
};

pub const JofAuditWorkspace = extern struct {
    records: ?[*]JofAuditRecord,
    record_cap: u32,
    tile: ?[*]u8,
    tile_cap: u32,
    scratch: ?[*]u8,
    scratch_cap: u32,
};

pub const JofAuditResult = extern struct {
    info: JofInfo,
    decoded_tiles: u32,
    coverage_errors: u32,
    geometry_errors: u32,
    duplicate_candidates: u32,
};

pub const JofProduceCfg = extern struct {
    pull: ?JofPullFn,
    pull_ctx: ?*anyopaque,
    sink: ?JofSinkFn,
    sink_ctx: ?*anyopaque,
    tile_w: u16,
    tile_h: u16,
    codec: u8,
    max_width: u16,
    max_height: u16,
    work: ?[*]u8,
    work_cap: usize,
    webp_work: ?[*]u8,
    webp_work_cap: usize,
};

pub const BookHeader = extern struct {
    magic: [8]u8,
    format_version: u32,
    total_size: u32,
    flags: u32,
    title_off: u32,
    author_off: u32,
    language_off: u32,
    identifier_off: u32,
    cover_image_index: u32,
    chapter_count: u32,
    chapter_off: u32,
    node_count: u32,
    node_off: u32,
    attr_count: u32,
    attr_off: u32,
    stylesheet_count: u32,
    stylesheet_off: u32,
    image_count: u32,
    image_off: u32,
    string_off: u32,
    string_size: u32,
    image_pool_off: u32,
    image_pool_size: u32,
    crc32_val: u32,
};

pub const BookChunked = extern struct {
    file_read: ?VsourceReadFn,
    file_ctx: ?*anyopaque,
    inflate_cb: ?BookInflateFn,
    table: ?[*]const u64,
    table_cap_entries: u32,
    staging: ?[*]u8,
    staging_cap: u32,
    payload_off: u64,
    inflated_total: u64,
    chunk_bytes: u32,
    chunk_count: u32,
};

pub const JofConvertRequirements = extern struct {
    width: u16,
    height: u16,
    tile_width: u16,
    tile_height: u16,
    work_bytes: u32,
    webp_work_bytes: u32,
};

pub const JofConvertWorkspace = extern struct {
    work: ?[*]u8,
    work_cap: u32,
    webp_work: ?[*]u8,
    webp_work_cap: u32,
};

pub const JofVerifyRequirements = extern struct {
    width: u16,
    height: u16,
    bpp: u8,
    band_height: u16,
    reference_work_bytes: u32,
    banded_work_bytes: u32,
    webp_work_bytes: u32,
    band_tile_bytes: u32,
    scratch_bytes: u32,
    row_bytes: u32,
};

pub const JofVerifyWorkspace = extern struct {
    work: ?[*]u8,
    work_cap: u32,
    webp_work: ?[*]u8,
    webp_work_cap: u32,
    band_tile: ?[*]u8,
    band_tile_cap: u32,
    scratch: ?[*]u8,
    scratch_cap: u32,
    row: ?[*]u8,
    row_cap: u32,
};

pub const JofInspectWorkspace = extern struct {
    records: ?[*]JofAuditRecord,
    record_cap: u32,
    tile: ?[*]u8,
    tile_cap: u32,
    scratch: ?[*]u8,
    scratch_cap: u32,
};

pub const RabookInspectWorkspace = extern struct {
    table: ?[*]u64,
    table_cap: u32,
    compressed: ?[*]u8,
    compressed_cap: u32,
    chunk: ?[*]u8,
    chunk_cap: u32,
    scratch: ?[*]u8,
    scratch_cap: u32,
};

comptime {
    if (@sizeOf(BookHeader) != 100) @compileError("BookHeader ABI size drift");
    if (@sizeOf(JofInfo) != 28) @compileError("JofInfo ABI size drift");
    if (@sizeOf(JofAuditRecord) != 24) @compileError("JofAuditRecord ABI size drift");
    if (@sizeOf(JofAuditResult) != 44) @compileError("JofAuditResult ABI size drift");
    if (@sizeOf(JofConvertRequirements) != 16) @compileError("JofConvertRequirements ABI size drift");
    if (@sizeOf(JofVerifyRequirements) != 32) @compileError("JofVerifyRequirements ABI size drift");
    if (@offsetOf(JofVerifyRequirements, "band_height") != 6) @compileError("JofVerifyRequirements band_height offset drift");
    if (@offsetOf(JofVerifyRequirements, "reference_work_bytes") != 8) @compileError("JofVerifyRequirements reference_work_bytes offset drift");
    const ptr = @sizeOf(?*anyopaque);
    if (@sizeOf(Sink) != ptr * 2) @compileError("Sink ABI size drift");
    if (@sizeOf(Spool) != ptr * 4) @compileError("Spool ABI size drift");
    if (@sizeOf(TransactionOps) != ptr * 3) @compileError("TransactionOps ABI size drift");
    if (@sizeOf(Transaction) != ptr * 2) @compileError("Transaction ABI size drift");
    if (@sizeOf(JofConvertWorkspace) != ptr * 4) @compileError("JofConvertWorkspace ABI size drift");
    if (@sizeOf(JofVerifyWorkspace) != ptr * 10) @compileError("JofVerifyWorkspace ABI size drift");
    if (@sizeOf(JofInspectWorkspace) != ptr * 6) @compileError("JofInspectWorkspace ABI size drift");
    if (@sizeOf(RabookInspectWorkspace) != ptr * 8) @compileError("RabookInspectWorkspace ABI size drift");
}

pub const Deps = struct {
    jof_work_bytes: *const fn (max_width: u16, max_height: u16, tile_w: u16, tile_h: u16) callconv(.c) u32,
    jof_webp_work_bytes: *const fn (max_width: u16, max_height: u16, max_src_bytes: u32) callconv(.c) u32,
    jof_produce: *const fn (cfg: *const JofProduceCfg, out_info: *JofInfo) callconv(.c) Ra8Err,
    jof_stored_bound: *const fn (raw_bytes: u32) callconv(.c) u32,
    jof_parse: *const fn (pread: ?PreadFn, pread_ctx: ?*anyopaque, total_size: u64, out_info: *JofInfo) callconv(.c) Ra8Err,
    jof_read_tile: *const fn (
        pread: ?PreadFn,
        pread_ctx: ?*anyopaque,
        info: *const JofInfo,
        tile_x: u16,
        tile_y: u16,
        scratch: ?[*]u8,
        scratch_cap: u32,
        out_px: ?[*]u8,
        out_cap: u32,
        out_w: *u16,
        out_h: *u16,
    ) callconv(.c) Ra8Err,
    jof_audit: *const fn (
        pread: ?PreadFn,
        pread_ctx: ?*anyopaque,
        total_size: u64,
        workspace: *JofAuditWorkspace,
        out: *JofAuditResult,
    ) callconv(.c) Ra8Err,
    book_chunked_open: *const fn (
        rd: *BookChunked,
        file_read: ?VsourceReadFn,
        file_ctx: ?*anyopaque,
        file_len: u64,
        inflate: ?BookInflateFn,
        table_buf: ?[*]u64,
        table_cap_entries: u32,
        staging: ?[*]u8,
        staging_cap: u32,
    ) callconv(.c) Ra8Err,
    book_chunked_validate_strict: *const fn (
        rd: *BookChunked,
        chunk: ?[*]u8,
        chunk_cap: u32,
        scratch: ?[*]u8,
        scratch_cap: u32,
        out_header: *BookHeader,
    ) callconv(.c) Ra8Err,
    ra8_imgdec_sniff: *const fn (bytes: [*]const u8, byte_count: u32, out: *u32) callconv(.c) Ra8Err,
    ra8_webp_get_info: *const fn (data: [*]const u8, size: usize, out_w: *u32, out_h: *u32) callconv(.c) Ra8Err,
    tinfl_decompress_mem_to_mem: *const fn (
        out_buf: ?*anyopaque,
        out_buf_len: usize,
        src_buf: ?*const anyopaque,
        src_buf_len: usize,
        flags: c_int,
    ) callconv(.c) usize,
};

extern fn jof_work_bytes(max_width: u16, max_height: u16, tile_w: u16, tile_h: u16) u32;
extern fn jof_webp_work_bytes(max_width: u16, max_height: u16, max_src_bytes: u32) u32;
extern fn jof_produce(cfg: *const JofProduceCfg, out_info: *JofInfo) Ra8Err;
extern fn jof_stored_bound(raw_bytes: u32) u32;
extern fn jof_parse(pread: ?PreadFn, pread_ctx: ?*anyopaque, total_size: u64, out_info: *JofInfo) Ra8Err;
extern fn jof_read_tile(
    pread: ?PreadFn,
    pread_ctx: ?*anyopaque,
    info: *const JofInfo,
    tile_x: u16,
    tile_y: u16,
    scratch: ?[*]u8,
    scratch_cap: u32,
    out_px: ?[*]u8,
    out_cap: u32,
    out_w: *u16,
    out_h: *u16,
) Ra8Err;
extern fn jof_audit(
    pread: ?PreadFn,
    pread_ctx: ?*anyopaque,
    total_size: u64,
    workspace: *JofAuditWorkspace,
    out: *JofAuditResult,
) Ra8Err;
extern fn book_chunked_open(
    rd: *BookChunked,
    file_read: ?VsourceReadFn,
    file_ctx: ?*anyopaque,
    file_len: u64,
    inflate: ?BookInflateFn,
    table_buf: ?[*]u64,
    table_cap_entries: u32,
    staging: ?[*]u8,
    staging_cap: u32,
) Ra8Err;
extern fn book_chunked_validate_strict(
    rd: *BookChunked,
    chunk: ?[*]u8,
    chunk_cap: u32,
    scratch: ?[*]u8,
    scratch_cap: u32,
    out_header: *BookHeader,
) Ra8Err;
extern fn ra8_imgdec_sniff(bytes: [*]const u8, byte_count: u32, out: *u32) Ra8Err;
extern fn ra8_webp_get_info(data: [*]const u8, size: usize, out_w: *u32, out_h: *u32) Ra8Err;
extern fn tinfl_decompress_mem_to_mem(
    out_buf: ?*anyopaque,
    out_buf_len: usize,
    src_buf: ?*const anyopaque,
    src_buf_len: usize,
    flags: c_int,
) usize;

pub const live_deps = Deps{
    .jof_work_bytes = jof_work_bytes,
    .jof_webp_work_bytes = jof_webp_work_bytes,
    .jof_produce = jof_produce,
    .jof_stored_bound = jof_stored_bound,
    .jof_parse = jof_parse,
    .jof_read_tile = jof_read_tile,
    .jof_audit = jof_audit,
    .book_chunked_open = book_chunked_open,
    .book_chunked_validate_strict = book_chunked_validate_strict,
    .ra8_imgdec_sniff = ra8_imgdec_sniff,
    .ra8_webp_get_info = ra8_webp_get_info,
    .tinfl_decompress_mem_to_mem = tinfl_decompress_mem_to_mem,
};
