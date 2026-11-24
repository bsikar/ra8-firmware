const __root = @This();

pub const ptrdiff_t = c_long;
pub const wchar_t = c_int;
pub const max_align_t = extern struct {
    __aro_max_align_ll: c_longlong,
    __aro_max_align_ld: c_longdouble,
};
pub const __u_char = u8;
pub const __u_short = c_ushort;
pub const __u_int = c_uint;
pub const __u_long = c_ulong;
pub const __int8_t = i8;
pub const __uint8_t = u8;
pub const __int16_t = c_short;
pub const __uint16_t = c_ushort;
pub const __int32_t = c_int;
pub const __uint32_t = c_uint;
pub const __int64_t = c_long;
pub const __uint64_t = c_ulong;
pub const __int_least8_t = __int8_t;
pub const __uint_least8_t = __uint8_t;
pub const __int_least16_t = __int16_t;
pub const __uint_least16_t = __uint16_t;
pub const __int_least32_t = __int32_t;
pub const __uint_least32_t = __uint32_t;
pub const __int_least64_t = __int64_t;
pub const __uint_least64_t = __uint64_t;
pub const __quad_t = c_long;
pub const __u_quad_t = c_ulong;
pub const __intmax_t = c_long;
pub const __uintmax_t = c_ulong;
pub const __dev_t = c_ulong;
pub const __uid_t = c_uint;
pub const __gid_t = c_uint;
pub const __ino_t = c_ulong;
pub const __ino64_t = c_ulong;
pub const __mode_t = c_uint;
pub const __nlink_t = c_ulong;
pub const __off_t = c_long;
pub const __off64_t = c_long;
pub const __pid_t = c_int;
pub const __fsid_t = extern struct {
    __val: [2]c_int,
};
pub const __clock_t = c_long;
pub const __rlim_t = c_ulong;
pub const __rlim64_t = c_ulong;
pub const __id_t = c_uint;
pub const __time_t = c_long;
pub const __useconds_t = c_uint;
pub const __suseconds_t = c_long;
pub const __suseconds64_t = c_long;
pub const __daddr_t = c_int;
pub const __key_t = c_int;
pub const __clockid_t = c_int;
pub const __timer_t = ?*anyopaque;
pub const __blksize_t = c_long;
pub const __blkcnt_t = c_long;
pub const __blkcnt64_t = c_long;
pub const __fsblkcnt_t = c_ulong;
pub const __fsblkcnt64_t = c_ulong;
pub const __fsfilcnt_t = c_ulong;
pub const __fsfilcnt64_t = c_ulong;
pub const __fsword_t = c_long;
pub const __ssize_t = c_long;
pub const __syscall_slong_t = c_long;
pub const __syscall_ulong_t = c_ulong;
pub const __loff_t = __off64_t;
pub const __caddr_t = [*c]u8;
pub const __intptr_t = c_long;
pub const __socklen_t = c_uint;
pub const __sig_atomic_t = c_int;
pub const int_least8_t = __int_least8_t;
pub const int_least16_t = __int_least16_t;
pub const int_least32_t = __int_least32_t;
pub const int_least64_t = __int_least64_t;
pub const uint_least8_t = __uint_least8_t;
pub const uint_least16_t = __uint_least16_t;
pub const uint_least32_t = __uint_least32_t;
pub const uint_least64_t = __uint_least64_t;
pub const int_fast8_t = i8;
pub const int_fast16_t = c_long;
pub const int_fast32_t = c_long;
pub const int_fast64_t = c_long;
pub const uint_fast8_t = u8;
pub const uint_fast16_t = c_ulong;
pub const uint_fast32_t = c_ulong;
pub const uint_fast64_t = c_ulong;
pub const intmax_t = __intmax_t;
pub const uintmax_t = __uintmax_t;
pub const k_ra8_ok: u16 = 0;
pub const k_ra8_fail: u16 = 257;
pub const k_ra8_err_no_mem: u16 = 258;
pub const k_ra8_err_invalid_arg: u16 = 259;
pub const k_ra8_err_invalid_state: u16 = 260;
pub const k_ra8_err_invalid_size: u16 = 261;
pub const k_ra8_err_not_found: u16 = 262;
pub const k_ra8_err_not_supported: u16 = 263;
pub const k_ra8_err_timeout: u16 = 264;
pub const k_ra8_err_busy: u16 = 265;
pub const k_ra8_err_no_data: u16 = 266;
pub const k_ra8_err_would_block: u16 = 267;
pub const k_ra8_err_exists: u16 = 268;
pub const k_ra8_err_empty: u16 = 269;
pub const k_ra8_err_cancelled: u16 = 270;
pub const k_ra8_err_not_initialized: u16 = 271;
pub const k_ra8_err_estop: u16 = 272;
pub const k_ra8_err_not_empty: u16 = 273;
pub const k_ra8_err_access_denied: u16 = 274;
pub const k_ra8_err_hw_init_failed: u16 = 513;
pub const k_ra8_err_hw_not_ready: u16 = 514;
pub const k_ra8_err_hw_timeout: u16 = 515;
pub const k_ra8_err_hw_error: u16 = 516;
pub const k_ra8_err_gpio_conflict: u16 = 517;
pub const k_ra8_err_gpio_invalid_port: u16 = 518;
pub const k_ra8_err_gpio_invalid_pin: u16 = 519;
pub const k_ra8_err_out_of_range: u16 = 520;
pub const k_ra8_err_hw_unmapped: u16 = 521;
pub const k_ra8_err_rtos_error: u16 = 769;
pub const k_ra8_err_rtos_thread_create: u16 = 770;
pub const k_ra8_err_rtos_semaphore: u16 = 771;
pub const k_ra8_err_rtos_mutex: u16 = 772;
pub const k_ra8_err_rtos_queue: u16 = 773;
pub const k_ra8_err_rtos_timer: u16 = 774;
pub const k_ra8_err_comm_error: u16 = 1025;
pub const k_ra8_err_spi_error: u16 = 1026;
pub const k_ra8_err_uart_error: u16 = 1027;
pub const k_ra8_err_i2c_error: u16 = 1028;
pub const k_ra8_err_crc_mismatch: u16 = 1029;
pub const k_ra8_err_protocol_error: u16 = 1030;
pub const k_ra8_err_nack: u16 = 1031;
pub const k_ra8_err_conflict: u16 = 1032;
pub const k_ra8_err_retry_limit: u16 = 1033;
pub const k_ra8_err_validation_failed: u16 = 1281;
pub const k_ra8_err_checksum_mismatch: u16 = 1282;
pub const k_ra8_err_range_check_failed: u16 = 1283;
pub const k_ra8_err_null_ptr: u16 = 1284;
pub const k_ra8_err_decomp_output_cap: u16 = 1285;
pub const k_ra8_err_decomp_ratio: u16 = 1286;
pub const k_ra8_err_decomp_entries: u16 = 1287;
pub const k_ra8_err_decomp_depth: u16 = 1288;
pub const k_ra8_err_decomp_iterations: u16 = 1289;
pub const ra8_err_codes_t = u16;
pub const ra8_err_t = ra8_err_codes_t;
pub fn ra8_err_is_error(arg_err: ra8_err_t) callconv(.c) bool {
    var err = arg_err;
    _ = &err;
    return @as(c_int, err) != @as(c_int, k_ra8_ok);
}
pub extern fn ra8_err_to_str(err: ra8_err_t) [*c]const u8;
pub const k_jof_hdr_bytes: u8 = 32;
pub const k_jof_footer_bytes: u8 = 16;
pub const k_jof_index_entry: u8 = 8;
pub const k_jof_magic_len: u8 = 4;
pub const k_jof_ofs_magic: u8 = 0;
pub const k_jof_ofs_width: u8 = 4;
pub const k_jof_ofs_height: u8 = 6;
pub const k_jof_ofs_tile_w: u8 = 8;
pub const k_jof_ofs_tile_h: u8 = 10;
pub const k_jof_ofs_bpp: u8 = 12;
pub const k_jof_ofs_codec: u8 = 13;
pub const k_jof_ofs_reserved: u8 = 14;
pub const k_jof_ofs_tile_count: u8 = 16;
pub const k_jof_ofs_reserved2: u8 = 20;
pub const k_jof_ftr_index_off: u8 = 0;
pub const k_jof_ftr_tile_count: u8 = 4;
pub const k_jof_ftr_total_size: u8 = 8;
pub const k_jof_ftr_magic: u8 = 12;
pub const k_jof_idx_ofs_offset: u8 = 0;
pub const k_jof_idx_ofs_length: u8 = 4;
pub const jof_layout_t = u8;
pub const k_jof_max_dim: c_uint = 32768;
pub const k_jof_max_tiles: c_uint = 65536;
pub const k_jof_bpp_max: c_uint = 4;
pub const jof_limits_t = u32;
pub const k_jof_codec_raw: u8 = 0;
pub const k_jof_codec_deflate: u8 = 1;
pub const jof_codec_t = u8;
pub const jof_info_t = extern struct {
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
    pub const jof_tile_dims = __root.jof_tile_dims;
    pub const dims = __root.jof_tile_dims;
};
pub const jof_pread_fn = ?*const fn (ctx: ?*anyopaque, offset: u64, buf: [*c]u8, len: usize, got: [*c]usize) callconv(.c) ra8_err_t;
pub const jof_memstore_t = extern struct {
    buf: [*c]u8,
    cap: usize,
    len: usize,
};
pub extern fn jof_memstore_sink(ctx: ?*anyopaque, buf: [*c]const u8, len: usize) ra8_err_t;
pub extern fn jof_memstore_pread(ctx: ?*anyopaque, offset: u64, buf: [*c]u8, len: usize, got: [*c]usize) ra8_err_t;
pub extern fn jof_parse(pread: jof_pread_fn, pread_ctx: ?*anyopaque, total_size: u64, out_info: [*c]jof_info_t) ra8_err_t;
pub extern fn jof_tile_dims(info: [*c]const jof_info_t, tile_x: u16, tile_y: u16, out_w: [*c]u16, out_h: [*c]u16) ra8_err_t;
pub extern fn jof_stored_bound(raw_bytes: u32) u32;
pub extern fn jof_read_tile(pread: jof_pread_fn, pread_ctx: ?*anyopaque, info: [*c]const jof_info_t, tile_x: u16, tile_y: u16, scratch: [*c]u8, scratch_cap: u32, out_px: [*c]u8, out_cap: u32, out_w: [*c]u16, out_h: [*c]u16) ra8_err_t;
pub const jof_pull_fn = ?*const fn (ctx: ?*anyopaque, buf: [*c]u8, cap: usize, got: [*c]usize) callconv(.c) ra8_err_t;
pub const jof_sink_fn = ?*const fn (ctx: ?*anyopaque, buf: [*c]const u8, len: usize) callconv(.c) ra8_err_t;
pub const jof_produce_cfg_t = extern struct {
    pull: jof_pull_fn,
    pull_ctx: ?*anyopaque,
    sink: jof_sink_fn,
    sink_ctx: ?*anyopaque,
    tile_w: u16,
    tile_h: u16,
    codec: u8,
    max_width: u16,
    max_height: u16,
    work: [*c]u8,
    work_cap: usize,
    webp_work: [*c]u8,
    webp_work_cap: usize,
    pub const jof_produce = __root.jof_produce;
    pub const produce = __root.jof_produce;
};
pub extern fn jof_work_bytes(max_width: u16, max_height: u16, tile_w: u16, tile_h: u16) u32;
pub extern fn jof_webp_work_bytes(max_width: u16, max_height: u16, max_src_bytes: u32) u32;
pub extern fn jof_probe_dims(data: [*c]const u8, len: usize, out_w: [*c]u16, out_h: [*c]u16) ra8_err_t;
pub extern fn jof_produce(cfg: [*c]const jof_produce_cfg_t, out_info: [*c]jof_info_t) ra8_err_t;
pub const k_jof_bump_align: u8 = 8;
pub const jof_bump_const_t = u8;
pub const jof_bump_t = extern struct {
    base: [*c]u8,
    cap: usize,
    off: usize,
    pub const priv_jof_bump_take = __root.priv_jof_bump_take;
    pub const take = __root.priv_jof_bump_take;
};
pub extern fn priv_jof_bump_take(bump: [*c]jof_bump_t, len: usize) ?*anyopaque;
pub const jof_geom_fn = ?*const fn (ctx: ?*anyopaque, width: u16, height: u16, channels: u8) callconv(.c) ra8_err_t;
pub const jof_rows_fn = ?*const fn (ctx: ?*anyopaque, px: [*c]const u8, width: u16, y0: u16, nrows: u16, channels: u8) callconv(.c) ra8_err_t;
pub extern fn priv_jof_png_rows(pull: jof_pull_fn, pull_ctx: ?*anyopaque, bump: [*c]jof_bump_t, max_w: u16, max_h: u16, on_geom: jof_geom_fn, on_rows: jof_rows_fn, cb_ctx: ?*anyopaque) ra8_err_t;
pub const k_jof_carve_slack: c_uint = 128;
pub const jof_carve_const_t = u32;
pub const jof_prefix_pull_t = extern struct {
    head: [*c]const u8,
    head_len: usize,
    pos: usize,
    inner: jof_pull_fn,
    inner_ctx: ?*anyopaque,
};
pub const jof_prod_state_t = extern struct {
    cfg: [*c]const jof_produce_cfg_t,
    bump_store: jof_bump_t,
    bump: [*c]jof_bump_t,
    cap_w: u16,
    cap_h: u16,
    w: u16,
    h: u16,
    bpp: u8,
    tile_cols: u16,
    tile_rows: u16,
    tile_count: u32,
    geom_done: u8,
    band: [*c]u8,
    stage: [*c]u8,
    cmp: [*c]u8,
    cmp_cap: u32,
    idx: [*c]u8,
    dfl: [*c]u8,
    dfl_len: u32,
    rows_seen: u32,
    band_fill: u32,
    tiles_done: u32,
    written: u32,
    pub const priv_jof_webp_transcode = __root.priv_jof_webp_transcode;
    pub const transcode = __root.priv_jof_webp_transcode;
};
pub extern fn priv_jof_prefix_pull(ctx: ?*anyopaque, buf: [*c]u8, cap: usize, got: [*c]usize) ra8_err_t;
pub extern fn priv_jof_on_geom(ctx: ?*anyopaque, width: u16, height: u16, channels: u8) ra8_err_t;
pub extern fn priv_jof_on_rows(ctx: ?*anyopaque, px: [*c]const u8, width: u16, y0: u16, nrows: u16, channels: u8) ra8_err_t;
pub extern fn priv_jof_webp_transcode(st: [*c]jof_prod_state_t, pfx: [*c]jof_prefix_pull_t) ra8_err_t;
pub const mz_uint8 = u8;
pub const mz_int8 = i8;
pub const mz_uint16 = u16;
pub const mz_int16 = i16;
pub const mz_uint32 = u32;
pub const mz_int32 = i32;
pub const mz_uint64 = u64;
pub const mz_int64 = i64;
pub const mz_uint = c_uint;
pub const mz_bool = c_int;
pub const mz_ulong = c_ulong;
pub const TINFL_FLAG_PARSE_ZLIB_HEADER: c_int = 1;
pub const TINFL_FLAG_HAS_MORE_INPUT: c_int = 2;
pub const TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF: c_int = 4;
pub const TINFL_FLAG_COMPUTE_ADLER32: c_int = 8;
const enum_unnamed_1 = c_uint;
pub const tinfl_status = mz_int32;
pub const TINFL_MAX_HUFF_TABLES: c_int = 3;
pub const TINFL_MAX_HUFF_SYMBOLS_0: c_int = 288;
pub const TINFL_MAX_HUFF_SYMBOLS_1: c_int = 32;
pub const TINFL_MAX_HUFF_SYMBOLS_2: c_int = 19;
pub const TINFL_FAST_LOOKUP_BITS: c_int = 10;
pub const TINFL_FAST_LOOKUP_SIZE: c_int = 1024;
const enum_unnamed_2 = c_uint;
pub const tinfl_bit_buf_t = mz_uint64;
pub const struct_tdefl_compressor = opaque {};
pub const tdefl_compressor = struct_tdefl_compressor;
pub const struct_tinfl_decompressor_tag = extern struct {
    m_state: mz_uint32,
    m_num_bits: mz_uint32,
    m_zhdr0: mz_uint32,
    m_zhdr1: mz_uint32,
    m_z_adler32: mz_uint32,
    m_final: mz_uint32,
    m_type: mz_uint32,
    m_check_adler32: mz_uint32,
    m_dist: mz_uint32,
    m_counter: mz_uint32,
    m_num_extra: mz_uint32,
    m_table_sizes: [3]mz_uint32,
    m_bit_buf: tinfl_bit_buf_t,
    m_dist_from_out_buf_start: usize,
    m_look_up: [3][1024]mz_int16,
    m_tree_0: [576]mz_int16,
    m_tree_1: [64]mz_int16,
    m_tree_2: [38]mz_int16,
    m_code_size_0: [288]mz_uint8,
    m_code_size_1: [32]mz_uint8,
    m_code_size_2: [19]mz_uint8,
    m_raw_header: [4]mz_uint8,
    m_len_codes: [457]mz_uint8,
    pub const tinfl_decompress = __root.tinfl_decompress;
    pub const decompress = __root.tinfl_decompress;
};
pub const tinfl_decompressor = struct_tinfl_decompressor_tag;
pub extern fn tinfl_decompress(r: [*c]tinfl_decompressor, pIn_buf_next: [*c]const mz_uint8, pIn_buf_size: [*c]usize, pOut_buf_start: [*c]mz_uint8, pOut_buf_next: [*c]mz_uint8, pOut_buf_size: [*c]usize, decomp_flags: mz_uint32) mz_int32;
pub const k_ra8_png_sig_bytes: c_uint = 8;
pub const k_ra8_png_chunk_hdr: c_uint = 8;
pub const k_ra8_png_crc_bytes: c_uint = 4;
pub const k_ra8_png_ihdr_len: c_uint = 13;
pub const k_ra8_png_max_chunks: c_uint = 4096;
pub const k_ra8_png_max_len: c_uint = 2147483647;
pub const k_ra8_png_ring_bytes: c_uint = 65536;
pub const k_ra8_png_inbuf_bytes: c_uint = 4096;
pub const k_ra8_png_plte_max: c_uint = 768;
pub const k_ra8_png_trns_max: c_uint = 256;
pub const k_ra8_png_skip_chunk: c_uint = 256;
pub const k_ra8_png_type_ihdr: c_uint = 1229472850;
pub const k_ra8_png_type_plte: c_uint = 1347179589;
pub const k_ra8_png_type_idat: c_uint = 1229209940;
pub const k_ra8_png_type_iend: c_uint = 1229278788;
pub const k_ra8_png_type_trns: c_uint = 1951551059;
pub const ra8_png_const_t = u32;
pub const k_ra8_png_sig_high: u8 = 137;
pub const k_ra8_png_sig_cr: u8 = 13;
pub const k_ra8_png_sig_lf: u8 = 10;
pub const k_ra8_png_sig_sub: u8 = 26;
pub const ra8_png_signature_t = u8;
pub const k_ra8_png_ihdr_ofs_depth: u8 = 8;
pub const k_ra8_png_ihdr_ofs_color: u8 = 9;
pub const k_ra8_png_ihdr_ofs_compress: u8 = 10;
pub const k_ra8_png_ihdr_ofs_filter: u8 = 11;
pub const k_ra8_png_ihdr_ofs_interlace: u8 = 12;
pub const k_ra8_png_depth_8: u8 = 8;
pub const k_ra8_png_color_gray: u8 = 0;
pub const k_ra8_png_color_rgb: u8 = 2;
pub const k_ra8_png_color_pal: u8 = 3;
pub const k_ra8_png_color_ga: u8 = 4;
pub const k_ra8_png_color_rgba: u8 = 6;
pub const k_ra8_png_filter_none: u8 = 0;
pub const k_ra8_png_filter_sub: u8 = 1;
pub const k_ra8_png_filter_up: u8 = 2;
pub const k_ra8_png_filter_avg: u8 = 3;
pub const k_ra8_png_filter_paeth: u8 = 4;
pub const k_ra8_png_ch_1: u8 = 1;
pub const k_ra8_png_ch_2: u8 = 2;
pub const k_ra8_png_ch_3: u8 = 3;
pub const k_ra8_png_ch_4: u8 = 4;
pub const k_ra8_png_be_sh24: u8 = 24;
pub const k_ra8_png_be_sh16: u8 = 16;
pub const k_ra8_png_be_sh8: u8 = 8;
pub const k_ra8_png_opaque: u8 = 255;
pub const ra8_png_field_t = u8;
pub const ra8_png_state_t = extern struct {
    pull: jof_pull_fn,
    pull_ctx: ?*anyopaque,
    on_geom: jof_geom_fn,
    on_rows: jof_rows_fn,
    cb_ctx: ?*anyopaque,
    w: u16,
    h: u16,
    color_type: u8,
    src_ch: u8,
    dst_ch: u8,
    palette: [768]u8,
    trns: [256]u8,
    plte_count: u16,
    trns_count: u16,
    has_plte: u8,
    has_trns: u8,
    inflator: [*c]tinfl_decompressor,
    ring: [*c]u8,
    inbuf: [*c]u8,
    rowbuf: [*c]u8,
    prevrow: [*c]u8,
    xlat: [*c]u8,
    ring_wr: u32,
    rowlen: u32,
    rowfill: u32,
    rows_done: u16,
    idat_rem: u32,
    source_done: u8,
    pending_valid: u8,
    pending_len: u32,
    pending_type: u32,
    pub const priv_jof_png_pull_exact = __root.priv_jof_png_pull_exact;
    pub const priv_jof_png_skip = __root.priv_jof_png_skip;
    pub const priv_jof_png_chunk_hdr = __root.priv_jof_png_chunk_hdr;
    pub const priv_jof_png_prologue = __root.priv_jof_png_prologue;
    pub const priv_jof_png_pre_idat = __root.priv_jof_png_pre_idat;
    pub const priv_jof_png_finish = __root.priv_jof_png_finish;
    pub const exact = __root.priv_jof_png_pull_exact;
    pub const skip = __root.priv_jof_png_skip;
    pub const hdr = __root.priv_jof_png_chunk_hdr;
    pub const prologue = __root.priv_jof_png_prologue;
    pub const idat = __root.priv_jof_png_pre_idat;
    pub const finish = __root.priv_jof_png_finish;
};
pub extern fn priv_jof_png_pull_exact(st: [*c]ra8_png_state_t, buf: [*c]u8, len: u32) ra8_err_t;
pub extern fn priv_jof_png_skip(st: [*c]ra8_png_state_t, len: u32) ra8_err_t;
pub extern fn priv_jof_png_chunk_hdr(st: [*c]ra8_png_state_t, out_len: [*c]u32, out_type: [*c]u32) ra8_err_t;
pub extern fn priv_jof_png_prologue(st: [*c]ra8_png_state_t, max_w: u16, max_h: u16) ra8_err_t;
pub extern fn priv_jof_png_pre_idat(st: [*c]ra8_png_state_t, len: u32, @"type": u32) ra8_err_t;
pub extern fn priv_jof_png_finish(st: [*c]ra8_png_state_t) ra8_err_t;
pub const struct_jof_audit_record_t = extern struct {
    offset: u32,
    length: u32,
    payload: u32,
    content_hash: u32,
    width: u16,
    height: u16,
    uniform: bool,
};
pub const jof_audit_record_t = struct_jof_audit_record_t;
pub const struct_jof_audit_requirements_t = extern struct {
    record_count: u32,
    tile_bytes: u32,
    scratch_bytes: u32,
};
pub const jof_audit_requirements_t = struct_jof_audit_requirements_t;
pub const struct_jof_audit_workspace_t = extern struct {
    records: [*c]jof_audit_record_t,
    record_cap: u32,
    tile: [*c]u8,
    tile_cap: u32,
    scratch: [*c]u8,
    scratch_cap: u32,
};
pub const jof_audit_workspace_t = struct_jof_audit_workspace_t;
pub const struct_jof_audit_result_t = extern struct {
    info: jof_info_t,
    decoded_tiles: u32,
    coverage_errors: u32,
    geometry_errors: u32,
    duplicate_candidates: u32,
};
pub const jof_audit_result_t = struct_jof_audit_result_t;
pub extern fn jof_audit_requirements(pread: jof_pread_fn, pread_ctx: ?*anyopaque, total_size: u64, out: [*c]jof_audit_requirements_t) ra8_err_t;
pub extern fn jof_audit(pread: jof_pread_fn, pread_ctx: ?*anyopaque, total_size: u64, workspace: [*c]jof_audit_workspace_t, out: [*c]jof_audit_result_t) ra8_err_t;
pub const ra8_arena_t = extern struct {
    base: [*c]u8,
    size: u32,
    used: u32,
    high_water: u32,
    pub const ra8_arena_init = __root.ra8_arena_init;
    pub const ra8_arena_carve = __root.ra8_arena_carve;
    pub const ra8_arena_remaining = __root.ra8_arena_remaining;
    pub const ra8_arena_carve_all = __root.ra8_arena_carve_all;
    pub const ra8_arena_carve_remaining = __root.ra8_arena_carve_remaining;
    pub const ra8_arena_high_water = __root.ra8_arena_high_water;
    pub const ra8_arena_reset = __root.ra8_arena_reset;
    pub const init = __root.ra8_arena_init;
    pub const carve = __root.ra8_arena_carve;
    pub const remaining = __root.ra8_arena_remaining;
    pub const all = __root.ra8_arena_carve_all;
    pub const water = __root.ra8_arena_high_water;
    pub const reset = __root.ra8_arena_reset;
};
pub extern fn ra8_arena_init(arena: [*c]ra8_arena_t, base: ?*anyopaque, size: u32) ra8_err_t;
pub extern fn ra8_arena_carve(arena: [*c]ra8_arena_t, bytes: u32, @"align": u32, out_ptr: [*c]?*anyopaque) ra8_err_t;
pub extern fn ra8_arena_remaining(arena: [*c]const ra8_arena_t, out_remaining: [*c]u32) ra8_err_t;
pub const ra8_arena_slot_t = extern struct {
    bytes: u32,
    @"align": u32,
    out_ptr: [*c]?*anyopaque,
};
pub const k_ra8_arena_slot_cap: c_uint = 16;
pub const ra8_arena_limits_t = u32;
pub extern fn ra8_arena_carve_all(arena: [*c]ra8_arena_t, slots: [*c]const ra8_arena_slot_t, slot_count: u32) ra8_err_t;
pub extern fn ra8_arena_carve_remaining(arena: [*c]ra8_arena_t, @"align": u32, out_ptr: [*c]?*anyopaque, out_bytes: [*c]u32) ra8_err_t;
pub extern fn ra8_arena_high_water(arena: [*c]const ra8_arena_t, out_high_water: [*c]u32) ra8_err_t;
pub extern fn ra8_arena_reset(arena: [*c]ra8_arena_t) ra8_err_t;
pub const k_ra8_imgdec_scratch_align: c_uint = 16;
pub const ra8_imgdec_scratch_limits_t = u32;
pub const ra8_imgdec_scratch_t = extern struct {
    base: [*c]u8,
    cap: usize,
    offset: usize,
    live: usize,
    high_water: usize,
    pub const ra8_imgdec_scratch_init = __root.ra8_imgdec_scratch_init;
    pub const ra8_imgdec_scratch_reset = __root.ra8_imgdec_scratch_reset;
    pub const ra8_imgdec_scratch_alloc = __root.ra8_imgdec_scratch_alloc;
    pub const ra8_imgdec_scratch_calloc = __root.ra8_imgdec_scratch_calloc;
    pub const ra8_imgdec_scratch_realloc = __root.ra8_imgdec_scratch_realloc;
    pub const ra8_imgdec_scratch_free = __root.ra8_imgdec_scratch_free;
    pub const ra8_imgdec_scratch_carve = __root.ra8_imgdec_scratch_carve;
    pub const ra8_imgdec_scratch_high_water = __root.ra8_imgdec_scratch_high_water;
    pub const ra8_webp_arena_bind = __root.ra8_webp_arena_bind;
    pub const init = __root.ra8_imgdec_scratch_init;
    pub const reset = __root.ra8_imgdec_scratch_reset;
    pub const alloc = __root.ra8_imgdec_scratch_alloc;
    pub const calloc = __root.ra8_imgdec_scratch_calloc;
    pub const realloc = __root.ra8_imgdec_scratch_realloc;
    pub const free = __root.ra8_imgdec_scratch_free;
    pub const carve = __root.ra8_imgdec_scratch_carve;
    pub const water = __root.ra8_imgdec_scratch_high_water;
    pub const bind = __root.ra8_webp_arena_bind;
};
pub extern fn ra8_imgdec_scratch_init(scratch: [*c]ra8_imgdec_scratch_t, base: ?*anyopaque, cap: usize) ra8_err_t;
pub extern fn ra8_imgdec_scratch_reset(scratch: [*c]ra8_imgdec_scratch_t) void;
pub extern fn ra8_imgdec_scratch_alloc(scratch: [*c]ra8_imgdec_scratch_t, bytes: usize) ?*anyopaque;
pub extern fn ra8_imgdec_scratch_calloc(scratch: [*c]ra8_imgdec_scratch_t, count: usize, size: usize) ?*anyopaque;
pub extern fn ra8_imgdec_scratch_realloc(scratch: [*c]ra8_imgdec_scratch_t, ptr: ?*anyopaque, old_bytes: usize, new_bytes: usize) ?*anyopaque;
pub extern fn ra8_imgdec_scratch_free(scratch: [*c]ra8_imgdec_scratch_t, ptr: ?*anyopaque) void;
pub extern fn ra8_imgdec_scratch_carve(scratch: [*c]ra8_imgdec_scratch_t, arena: [*c]ra8_arena_t, bytes: u32, @"align": u32) ra8_err_t;
pub extern fn ra8_imgdec_scratch_high_water(scratch: [*c]const ra8_imgdec_scratch_t) usize;
pub const ra8_webp_arena_t = ra8_imgdec_scratch_t;
pub extern fn ra8_webp_arena_bind(arena: [*c]ra8_webp_arena_t) void;
pub extern fn ra8_webp_arena_unbind() void;
pub extern fn ra8_webp_arena_malloc(n: usize) ?*anyopaque;
pub extern fn ra8_webp_arena_calloc(nmemb: usize, size: usize) ?*anyopaque;
pub extern fn ra8_webp_arena_free(p: ?*anyopaque) void;
pub const k_ra8_webp_max_dim: c_uint = 8192;
pub const k_ra8_webp_bytes_per_px: c_uint = 4;
pub const ra8_webp_limits_t = u32;
pub extern fn ra8_webp_get_info(data: [*c]const u8, size: usize, out_w: [*c]u32, out_h: [*c]u32) ra8_err_t;
pub extern fn ra8_webp_decode_rgba(data: [*c]const u8, size: usize, arena: [*c]ra8_webp_arena_t, out_rgba: [*c]u8, out_stride: usize, out_capacity: usize, out_w: [*c]u32, out_h: [*c]u32) ra8_err_t;
pub const TINFL_STATUS_DONE = @as(c_int, 0);
pub const tinfl_decompressor_tag = struct_tinfl_decompressor_tag;
