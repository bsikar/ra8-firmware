const __root = @This();

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
pub const ptrdiff_t = c_long;
pub const wchar_t = c_int;
pub const max_align_t = extern struct {
    __aro_max_align_ll: c_longlong,
    __aro_max_align_ld: c_longdouble,
};
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
pub const k_ra8_keycache_evict_lru: u8 = 0;
pub const k_ra8_keycache_evict_slru: u8 = 1;
pub const ra8_keycache_evict_t = u8;
pub const ra8_keycache_hash_fn = ?*const fn (key: ?*const anyopaque, key_bytes: u32, ctx: ?*anyopaque) callconv(.c) u32;
pub const ra8_keycache_render_fn = ?*const fn (ctx: ?*anyopaque, key: ?*const anyopaque, cell: [*c]u8, cell_bytes: u32, user: ?*anyopaque) callconv(.c) ra8_err_t;
pub const ra8_keycache_cell_t = extern struct {
    prev: i32,
    next: i32,
    hash_next: i32,
    pin_count: u16,
    seg: u8,
    valid: u8,
};
pub const ra8_keycache_cfg_t = extern struct {
    cell_mem: [*c]u8,
    cell_bytes: u32,
    cell_count: u32,
    key_mem: [*c]u8,
    key_bytes: u32,
    user_mem: [*c]u8,
    user_bytes: u32,
    meta: [*c]ra8_keycache_cell_t,
    buckets: [*c]i32,
    bucket_count: u32,
    render: ra8_keycache_render_fn,
    render_ctx: ?*anyopaque,
    evict: ra8_keycache_evict_t,
    protected_pct: u8,
    hash: ra8_keycache_hash_fn,
    hash_ctx: ?*anyopaque,
};
pub const ra8_keycache_t = extern struct {
    cfg: ra8_keycache_cfg_t,
    pb_head: i32,
    pb_tail: i32,
    pt_head: i32,
    pt_tail: i32,
    protected_count: u32,
    protected_cap: u32,
    hits: u32,
    misses: u32,
    evictions: u32,
    pub const ra8_keycache_init = __root.ra8_keycache_init;
    pub const ra8_keycache_get = __root.ra8_keycache_get;
    pub const ra8_keycache_prefetch = __root.ra8_keycache_prefetch;
    pub const ra8_keycache_put = __root.ra8_keycache_put;
    pub const ra8_keycache_stats = __root.ra8_keycache_stats;
    pub const init = __root.ra8_keycache_init;
    pub const get = __root.ra8_keycache_get;
    pub const prefetch = __root.ra8_keycache_prefetch;
    pub const put = __root.ra8_keycache_put;
    pub const stats = __root.ra8_keycache_stats;
};
pub const ra8_keycache_view_t = extern struct {
    data: [*c]u8,
    user: ?*anyopaque,
};
pub extern fn ra8_keycache_init(kc: [*c]ra8_keycache_t, cfg: [*c]const ra8_keycache_cfg_t) ra8_err_t;
pub extern fn ra8_keycache_get(kc: [*c]ra8_keycache_t, key: ?*const anyopaque, out_view: [*c]ra8_keycache_view_t) ra8_err_t;
pub extern fn ra8_keycache_prefetch(kc: [*c]ra8_keycache_t, key: ?*const anyopaque) ra8_err_t;
pub extern fn ra8_keycache_put(kc: [*c]ra8_keycache_t, data: [*c]const u8) ra8_err_t;
pub extern fn ra8_keycache_stats(kc: [*c]const ra8_keycache_t, out_hits: [*c]u32, out_misses: [*c]u32, out_evictions: [*c]u32) ra8_err_t;
pub const ra8_tile_key_t = extern struct {
    image_id: u32,
    tile_x: u16,
    tile_y: u16,
    zoom: u16,
    reserved: u16,
};
comptime {
    if (!(@sizeOf(ra8_tile_key_t) == ((((@sizeOf(u32) +% @sizeOf(u16)) +% @sizeOf(u16)) +% @sizeOf(u16)) +% @sizeOf(u16)))) @compileError("static assertion failed \"ra8_tile_key_t must be padding-free for byte-wise key comparison\"");
}
pub const ra8_tile_dims_t = extern struct {
    w: u16,
    h: u16,
};
pub const ra8_tile_t = extern struct {
    pixels: [*c]const u8,
    width: u16,
    height: u16,
};
pub const ra8_tile_decode_fn = ?*const fn (ctx: ?*anyopaque, key: [*c]const ra8_tile_key_t, cell: [*c]u8, cell_bytes: u32, out_w: [*c]u16, out_h: [*c]u16) callconv(.c) ra8_err_t;
pub const ra8_tile_cache_cfg_t = extern struct {
    cell_mem: [*c]u8,
    cell_bytes: u32,
    cell_count: u32,
    meta: [*c]ra8_keycache_cell_t,
    keys: [*c]ra8_tile_key_t,
    dims: [*c]ra8_tile_dims_t,
    buckets: [*c]i32,
    bucket_count: u32,
    decode: ra8_tile_decode_fn,
    decode_ctx: ?*anyopaque,
};
pub const ra8_tile_cache_t = extern struct {
    kc: ra8_keycache_t,
    decode: ra8_tile_decode_fn,
    decode_ctx: ?*anyopaque,
    pub const ra8_tile_cache_init = __root.ra8_tile_cache_init;
    pub const ra8_tile_cache_get = __root.ra8_tile_cache_get;
    pub const ra8_tile_cache_put = __root.ra8_tile_cache_put;
    pub const ra8_tile_cache_capacity = __root.ra8_tile_cache_capacity;
    pub const ra8_tile_cache_prefetch = __root.ra8_tile_cache_prefetch;
    pub const ra8_tile_cache_prefetch_pan = __root.ra8_tile_cache_prefetch_pan;
    pub const ra8_tile_cache_stats = __root.ra8_tile_cache_stats;
    pub const init = __root.ra8_tile_cache_init;
    pub const get = __root.ra8_tile_cache_get;
    pub const put = __root.ra8_tile_cache_put;
    pub const capacity = __root.ra8_tile_cache_capacity;
    pub const prefetch = __root.ra8_tile_cache_prefetch;
    pub const pan = __root.ra8_tile_cache_prefetch_pan;
    pub const stats = __root.ra8_tile_cache_stats;
};
pub const k_ra8_tile_pan_none: u8 = 0;
pub const k_ra8_tile_pan_left: u8 = 1;
pub const k_ra8_tile_pan_right: u8 = 2;
pub const k_ra8_tile_pan_up: u8 = 3;
pub const k_ra8_tile_pan_down: u8 = 4;
pub const ra8_tile_pan_dir_t = u8;
pub const ra8_tile_rect_t = extern struct {
    tx0: u16,
    ty0: u16,
    tx1: u16,
    ty1: u16,
};
pub const ra8_tile_prefetch_req_t = extern struct {
    image_id: u32,
    view: ra8_tile_rect_t,
    tile_cols: u16,
    tile_rows: u16,
    zoom: u16,
    max_tiles: u16,
    dir: ra8_tile_pan_dir_t,
};
pub extern fn ra8_tile_rect_of_pixels(px: u32, py: u32, pw: u32, ph: u32, tile_w: u16, tile_h: u16, tile_cols: u16, tile_rows: u16, out: [*c]ra8_tile_rect_t) ra8_err_t;
pub extern fn ra8_tile_cache_init(tc: [*c]ra8_tile_cache_t, cfg: [*c]const ra8_tile_cache_cfg_t) ra8_err_t;
pub extern fn ra8_tile_cache_get(tc: [*c]ra8_tile_cache_t, key: [*c]const ra8_tile_key_t, out_tile: [*c]ra8_tile_t) ra8_err_t;
pub extern fn ra8_tile_cache_put(tc: [*c]ra8_tile_cache_t, pixels: [*c]const u8) ra8_err_t;
pub extern fn ra8_tile_cache_capacity(tc: [*c]const ra8_tile_cache_t) u32;
pub extern fn ra8_tile_cache_prefetch(tc: [*c]ra8_tile_cache_t, key: [*c]const ra8_tile_key_t) ra8_err_t;
pub extern fn ra8_tile_cache_prefetch_pan(tc: [*c]ra8_tile_cache_t, req: [*c]const ra8_tile_prefetch_req_t, out_warmed: [*c]u16) ra8_err_t;
pub extern fn ra8_tile_cache_stats(tc: [*c]const ra8_tile_cache_t, out_hits: [*c]u32, out_misses: [*c]u32, out_evictions: [*c]u32) ra8_err_t;
pub const k_longstrip_max_visible_bands: u16 = 4096;
pub const k_longstrip_max_prefetch: u16 = 8;
pub const longstrip_limits_t = u16;
pub const longstrip_blit_fn = ?*const fn (ctx: ?*anyopaque, pixels: [*c]const u8, src_w: u16, src_h: u16, bpp: u8, dst_x: i32, dst_y: i32) callconv(.c) ra8_err_t;
pub const longstrip_decode_ctx_t = extern struct {
    pread: jof_pread_fn,
    pread_ctx: ?*anyopaque,
    info: jof_info_t,
    scratch: [*c]u8,
    scratch_cap: u32,
};
pub const longstrip_cfg_t = extern struct {
    pread: jof_pread_fn,
    pread_ctx: ?*anyopaque,
    atlas_size: u64,
    cache: [*c]ra8_tile_cache_t,
    image_id: u32,
    viewport_w: u16,
    viewport_h: u16,
    blit: longstrip_blit_fn,
    blit_ctx: ?*anyopaque,
};
pub const longstrip_t = extern struct {
    info: jof_info_t,
    canvas_w: u32,
    canvas_h: u32,
    band_h: u16,
    band_count: u16,
    viewport_w: u16,
    viewport_h: u16,
    scroll_y: i32,
    max_scroll: i32,
    velocity: i32,
    cache: [*c]ra8_tile_cache_t,
    image_id: u32,
    blit: longstrip_blit_fn,
    blit_ctx: ?*anyopaque,
    pub const longstrip_open = __root.longstrip_open;
    pub const longstrip_set_viewport = __root.longstrip_set_viewport;
    pub const longstrip_clamp_scroll = __root.longstrip_clamp_scroll;
    pub const longstrip_band_at_y = __root.longstrip_band_at_y;
    pub const longstrip_visible_bands = __root.longstrip_visible_bands;
    pub const longstrip_scroll_by = __root.longstrip_scroll_by;
    pub const longstrip_fling = __root.longstrip_fling;
    pub const longstrip_tick = __root.longstrip_tick;
    pub const longstrip_prefetch = __root.longstrip_prefetch;
    pub const longstrip_render = __root.longstrip_render;
    pub const open = __root.longstrip_open;
    pub const viewport = __root.longstrip_set_viewport;
    pub const scroll = __root.longstrip_clamp_scroll;
    pub const y = __root.longstrip_band_at_y;
    pub const bands = __root.longstrip_visible_bands;
    pub const by = __root.longstrip_scroll_by;
    pub const fling = __root.longstrip_fling;
    pub const tick = __root.longstrip_tick;
    pub const prefetch = __root.longstrip_prefetch;
    pub const render = __root.longstrip_render;
};
pub const longstrip_render_stats_t = extern struct {
    bands_drawn: u16,
    skipped: u16,
    covered_rows: u32,
};
pub const longstrip_clip_rect_t = extern struct {
    src_x: u16,
    src_y: u16,
    dst_x: i32,
    dst_y: i32,
    width: u16,
    height: u16,
    visible: bool,
};
pub const longstrip_hw_blit_fn = ?*const fn (ctx: ?*anyopaque, pixels: [*c]const u8, src_w: u16, src_h: u16, bpp: u8, dst_x: i32, dst_y: i32, view_w: u16, view_h: u16) callconv(.c) ra8_err_t;
pub const longstrip_hw_fill_fn = ?*const fn (ctx: ?*anyopaque, x: i32, y: i32, w: u16, h: u16, argb8888: u32) callconv(.c) ra8_err_t;
pub const longstrip_hw_ops_t = extern struct {
    blit_band: longstrip_hw_blit_fn,
    fill_rect: longstrip_hw_fill_fn,
    ctx: ?*anyopaque,
};
pub const longstrip_target_t = extern struct {
    fb565: [*c]u16,
    fb_pitch_px: u16,
    view_w: u16,
    view_h: u16,
    x_off: i32,
    prefer_hw: bool,
    hw: longstrip_hw_ops_t,
};
pub extern fn longstrip_tile_decode(ctx: ?*anyopaque, key: [*c]const ra8_tile_key_t, cell: [*c]u8, cell_bytes: u32, out_w: [*c]u16, out_h: [*c]u16) ra8_err_t;
pub extern fn longstrip_open(wt: [*c]longstrip_t, cfg: [*c]const longstrip_cfg_t) ra8_err_t;
pub extern fn longstrip_set_viewport(wt: [*c]longstrip_t, viewport_w: u16, viewport_h: u16) ra8_err_t;
pub extern fn longstrip_clamp_scroll(wt: [*c]const longstrip_t, y: i32) i32;
pub extern fn longstrip_band_at_y(wt: [*c]const longstrip_t, y: u32, out_band: [*c]u16) ra8_err_t;
pub extern fn longstrip_visible_bands(wt: [*c]const longstrip_t, scroll_y: i32, out_first: [*c]u16, out_last: [*c]u16) ra8_err_t;
pub extern fn longstrip_scroll_by(wt: [*c]longstrip_t, delta: i32) ra8_err_t;
pub extern fn longstrip_fling(wt: [*c]longstrip_t, v0: i32) ra8_err_t;
pub extern fn longstrip_tick(wt: [*c]longstrip_t) bool;
pub extern fn longstrip_prefetch(wt: [*c]longstrip_t, depth: u16) ra8_err_t;
pub extern fn longstrip_render(wt: [*c]longstrip_t, stats: [*c]longstrip_render_stats_t) ra8_err_t;
pub extern fn longstrip_clip_band(dst_x: i32, dst_y: i32, src_w: u16, src_h: u16, view_w: u16, view_h: u16, out: [*c]longstrip_clip_rect_t) ra8_err_t;
pub extern fn longstrip_sw_blit_rgb565(fb565: [*c]u16, fb_pitch_px: u16, view_w: u16, view_h: u16, pixels: [*c]const u8, src_w: u16, src_h: u16, bpp: u8, dst_x: i32, dst_y: i32) ra8_err_t;
pub extern fn longstrip_target_blit(target_ctx: ?*anyopaque, pixels: [*c]const u8, src_w: u16, src_h: u16, bpp: u8, dst_x: i32, dst_y: i32) ra8_err_t;
pub extern fn longstrip_target_fill(target: [*c]const longstrip_target_t, x: i32, y: i32, w: u16, h: u16, argb8888: u32) ra8_err_t;

pub const __VERSION__ = "Aro 0.0.0";
pub const __Aro__ = "";
pub const __STDC__ = @as(c_int, 1);
pub const __STDC_HOSTED__ = @as(c_int, 1);
pub const __STDC_UTF_16__ = @as(c_int, 1);
pub const __STDC_UTF_32__ = @as(c_int, 1);
pub const __STDC_EMBED_NOT_FOUND__ = @as(c_int, 0);
pub const __STDC_EMBED_FOUND__ = @as(c_int, 1);
pub const __STDC_EMBED_EMPTY__ = @as(c_int, 2);
pub const __STDC_VERSION__ = @as(c_long, 201710);
pub const __GNUC__ = @as(c_int, 7);
pub const __GNUC_MINOR__ = @as(c_int, 1);
pub const __GNUC_PATCHLEVEL__ = @as(c_int, 0);
pub const __ARO_EMULATE_NO__ = @as(c_int, 0);
pub const __ARO_EMULATE_CLANG__ = @as(c_int, 1);
pub const __ARO_EMULATE_GCC__ = @as(c_int, 2);
pub const __ARO_EMULATE_MSVC__ = @as(c_int, 3);
pub const __ARO_EMULATE__ = __ARO_EMULATE_NO__;
pub inline fn __building_module(x: anytype) @TypeOf(@as(c_int, 0)) {
    _ = &x;
    return @as(c_int, 0);
}
pub const TARGET_OS_WIN32 = @as(c_int, 0);
pub const TARGET_OS_WINDOWS = @as(c_int, 0);
pub const TARGET_OS_LINUX = @as(c_int, 1);
pub const TARGET_OS_UNIX = @as(c_int, 0);
pub const TARGET_OS_MAC = @as(c_int, 0);
pub const TARGET_OS_OSX = @as(c_int, 0);
pub const TARGET_OS_IPHONE = @as(c_int, 0);
pub const TARGET_OS_IOS = @as(c_int, 0);
pub const TARGET_OS_TV = @as(c_int, 0);
pub const TARGET_OS_WATCH = @as(c_int, 0);
pub const TARGET_OS_VISION = @as(c_int, 0);
pub const TARGET_OS_DRIVERKIT = @as(c_int, 0);
pub const TARGET_OS_MACCATALYST = @as(c_int, 0);
pub const TARGET_OS_SIMULATOR = @as(c_int, 0);
pub const TARGET_OS_EMBEDDED = @as(c_int, 0);
pub const TARGET_OS_NANO = @as(c_int, 0);
pub const TARGET_IPHONE_SIMULATOR = @as(c_int, 0);
pub const TARGET_OS_UIKITFORMAC = @as(c_int, 0);
pub const TARGET_OS_UEFI = @as(c_int, 0);
pub const linux = @as(c_int, 1);
pub const __linux = @as(c_int, 1);
pub const __linux__ = @as(c_int, 1);
pub const unix = @as(c_int, 1);
pub const __unix = @as(c_int, 1);
pub const __unix__ = @as(c_int, 1);
pub const __code_model_small__ = @as(c_int, 1);
pub const __amd64__ = @as(c_int, 1);
pub const __amd64 = @as(c_int, 1);
pub const __x86_64__ = @as(c_int, 1);
pub const __x86_64 = @as(c_int, 1);
pub const __SEG_GS = @as(c_int, 1);
pub const __SEG_FS = @as(c_int, 1);
pub const __seg_gs = @compileError("unable to translate macro: undefined identifier `address_space`");
// <builtin>:52:9
pub const __seg_fs = @compileError("unable to translate macro: undefined identifier `address_space`");
// <builtin>:53:9
pub const __LAHF_SAHF__ = @as(c_int, 1);
pub const __AES__ = @as(c_int, 1);
pub const __VAES__ = @as(c_int, 1);
pub const __PCLMUL__ = @as(c_int, 1);
pub const __VPCLMULQDQ__ = @as(c_int, 1);
pub const __LZCNT__ = @as(c_int, 1);
pub const __RDRND__ = @as(c_int, 1);
pub const __FSGSBASE__ = @as(c_int, 1);
pub const __BMI__ = @as(c_int, 1);
pub const __BMI2__ = @as(c_int, 1);
pub const __POPCNT__ = @as(c_int, 1);
pub const __PRFCHW__ = @as(c_int, 1);
pub const __RDSEED__ = @as(c_int, 1);
pub const __ADX__ = @as(c_int, 1);
pub const __MOVBE__ = @as(c_int, 1);
pub const __SSE4A__ = @as(c_int, 1);
pub const __FMA__ = @as(c_int, 1);
pub const __F16C__ = @as(c_int, 1);
pub const __GFNI__ = @as(c_int, 1);
pub const __AVX512CD__ = @as(c_int, 1);
pub const __AVX512VPOPCNTDQ__ = @as(c_int, 1);
pub const __AVX512VNNI__ = @as(c_int, 1);
pub const __AVX512BF16__ = @as(c_int, 1);
pub const __AVX512DQ__ = @as(c_int, 1);
pub const __AVX512BITALG__ = @as(c_int, 1);
pub const __AVX512BW__ = @as(c_int, 1);
pub const __AVX512VL__ = @as(c_int, 1);
pub const __EVEX256__ = @as(c_int, 1);
pub const __AVX512VBMI__ = @as(c_int, 1);
pub const __AVX512VBMI2__ = @as(c_int, 1);
pub const __AVX512IFMA__ = @as(c_int, 1);
pub const __SHA__ = @as(c_int, 1);
pub const __FXSR__ = @as(c_int, 1);
pub const __XSAVE__ = @as(c_int, 1);
pub const __XSAVEOPT__ = @as(c_int, 1);
pub const __XSAVEC__ = @as(c_int, 1);
pub const __XSAVES__ = @as(c_int, 1);
pub const __CLFLUSHOPT__ = @as(c_int, 1);
pub const __CLWB__ = @as(c_int, 1);
pub const __SHSTK__ = @as(c_int, 1);
pub const __CLZERO__ = @as(c_int, 1);
pub const __RDPID__ = @as(c_int, 1);
pub const __INVPCID__ = @as(c_int, 1);
pub const __CRC32__ = @as(c_int, 1);
pub const __AVX512F__ = @as(c_int, 1);
pub const __AVX2__ = @as(c_int, 1);
pub const __AVX__ = @as(c_int, 1);
pub const __SSE4_2__ = @as(c_int, 1);
pub const __SSE4_1__ = @as(c_int, 1);
pub const __SSSE3__ = @as(c_int, 1);
pub const __SSE3__ = @as(c_int, 1);
pub const __SSE2__ = @as(c_int, 1);
pub const __SSE__ = @as(c_int, 1);
pub const __SSE_MATH__ = @as(c_int, 1);
pub const __MMX__ = @as(c_int, 1);
pub const __GCC_HAVE_SYNC_COMPARE_AND_SWAP_8 = @as(c_int, 1);
pub const __SIZEOF_FLOAT128__ = @as(c_int, 16);
pub const _LP64 = @as(c_int, 1);
pub const __LP64__ = @as(c_int, 1);
pub const __FLOAT128__ = @as(c_int, 1);
pub const __ORDER_LITTLE_ENDIAN__ = @as(c_int, 1234);
pub const __ORDER_BIG_ENDIAN__ = @as(c_int, 4321);
pub const __ORDER_PDP_ENDIAN__ = @as(c_int, 3412);
pub const __BYTE_ORDER__ = __ORDER_LITTLE_ENDIAN__;
pub const __LITTLE_ENDIAN__ = @as(c_int, 1);
pub const __ELF__ = @as(c_int, 1);
pub const __ATOMIC_RELAXED = @as(c_int, 0);
pub const __ATOMIC_CONSUME = @as(c_int, 1);
pub const __ATOMIC_ACQUIRE = @as(c_int, 2);
pub const __ATOMIC_RELEASE = @as(c_int, 3);
pub const __ATOMIC_ACQ_REL = @as(c_int, 4);
pub const __ATOMIC_SEQ_CST = @as(c_int, 5);
pub const __ATOMIC_BOOL_LOCK_FREE = @as(c_int, 1);
pub const __ATOMIC_CHAR_LOCK_FREE = @as(c_int, 1);
pub const __ATOMIC_CHAR16_T_LOCK_FREE = @as(c_int, 1);
pub const __ATOMIC_CHAR32_T_LOCK_FREE = @as(c_int, 1);
pub const __ATOMIC_WCHAR_T_LOCK_FREE = @as(c_int, 1);
pub const __ATOMIC_WINT_T_LOCK_FREE = @as(c_int, 1);
pub const __ATOMIC_SHORT_LOCK_FREE = @as(c_int, 1);
pub const __ATOMIC_INT_LOCK_FREE = @as(c_int, 1);
pub const __ATOMIC_LONG_LOCK_FREE = @as(c_int, 1);
pub const __ATOMIC_LLONG_LOCK_FREE = @as(c_int, 1);
pub const __ATOMIC_POINTER_LOCK_FREE = @as(c_int, 1);
pub const __WINT_UNSIGNED__ = @as(c_int, 1);
pub const __CHAR_BIT__ = @as(c_int, 8);
pub const __BOOL_WIDTH__ = @as(c_int, 8);
pub const __SCHAR_MAX__ = @as(c_int, 127);
