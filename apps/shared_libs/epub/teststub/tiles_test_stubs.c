/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Brighton Sikarskie
 *
 * Test-only stand-ins for the extern surface of src/epub_img_tiles_abi.zig
 * (RA8FW-1194): the entry-cursor trio, epub_get_resource, jof_parse,
 * jof_read_tile and the tile-cache quartet. Compiled into the package test
 * module only; production links the real ones.
 */
#include <stdint.h>
#include <stddef.h>

static void ts_memset(void *dst, uint8_t value, size_t n) {
  uint8_t *d = (uint8_t *)dst;
  for (size_t i = 0; i < n; ++i) d[i] = value;
}

static void ts_memcpy(void *dst, const void *src, size_t n) {
  const uint8_t *s = (const uint8_t *)src;
  uint8_t *d = (uint8_t *)dst;
  for (size_t i = 0; i < n; ++i) d[i] = s[i];
}

static int ts_strstr_has(const char *hay, const char *needle) {
  for (size_t i = 0; hay[i] != '\0'; ++i) {
    size_t j = 0;
    while (needle[j] != '\0' && hay[i + j] == needle[j]) ++j;
    if (needle[j] == '\0') return 1;
  }
  return 0;
}

typedef uint16_t ra8_err_t;
#define k_ra8_ok             ((ra8_err_t)0)
#define k_ra8_err_no_mem     ((ra8_err_t)0x102)
#define k_ra8_err_invalid_size ((ra8_err_t)0x105)
#define k_ra8_err_not_found  ((ra8_err_t)0x106)

typedef ra8_err_t (*jof_pread_fn)(void *ctx, uint64_t offset, uint8_t *buf, size_t len, size_t *got);

typedef struct {
  uint16_t width;
  uint16_t height;
  uint16_t tile_w;
  uint16_t tile_h;
  uint16_t tile_cols;
  uint16_t tile_rows;
  uint8_t  bpp;
  uint8_t  codec;
  uint32_t tile_count;
  uint32_t index_off;
  uint32_t total_size;
} jof_info_t;

typedef struct {
  uint32_t image_id;
  uint16_t tile_x;
  uint16_t tile_y;
  uint16_t zoom;
  uint16_t reserved;
} ra8_tile_key_t;

typedef struct {
  const uint8_t *pixels;
  uint16_t       width;
  uint16_t       height;
} ra8_tile_t;

typedef ra8_err_t (*ra8_tile_decode_fn)(void *ctx, const ra8_tile_key_t *key, uint8_t *cell,
                                        uint32_t cell_bytes, uint16_t *out_w, uint16_t *out_h);

typedef struct {
  uint8_t             *cell_mem;
  uint32_t             cell_bytes;
  uint32_t             cell_count;
  void                *meta;
  void                *keys;
  void                *dims;
  int32_t             *buckets;
  uint32_t             bucket_count;
  ra8_tile_decode_fn   decode;
  void                *decode_ctx;
} ra8_tile_cache_cfg_t;

typedef struct {
  ra8_tile_cache_cfg_t cfg;
  int32_t  pb_head;
  int32_t  pb_tail;
  int32_t  pt_head;
  int32_t  pt_tail;
  uint32_t protected_count;
  uint32_t protected_cap;
  uint32_t hits;
  uint32_t misses;
  uint32_t evictions;
} ra8_tile_cache_t;

typedef ra8_err_t (*epub_entry_reader_shim_t)(void);

/* The stub "book" is any non-NULL pointer; the entry trio answers a fixed
 * 64-byte atlas for every path except "missing.jof", which measures
 * not_found so the binder's failed-measure path is exercised. */
static const uint8_t s_atlas[64] = {0};

ra8_err_t epub_entry_open(void *book, const char *path, void *out_reader, uint64_t *out_size)
{
  (void)book;
  (void)out_reader;
  if (ts_strstr_has(path, "missing") != 0) {
    return k_ra8_err_not_found;
  }
  *out_size = 64;
  return k_ra8_ok;
}

ra8_err_t epub_entry_close(void *reader)
{
  (void)reader;
  return k_ra8_ok;
}

ra8_err_t epub_entry_pread(void *book, const char *path, uint64_t offset, uint8_t *buf, size_t len, size_t *got)
{
  (void)book;
  (void)path;
  if (offset >= 64) {
    *got = 0;
    return k_ra8_ok;
  }
  size_t avail = (size_t)(64 - offset);
  size_t n     = (len < avail) ? len : avail;
  ts_memcpy(buf, s_atlas + offset, n);
  *got = n;
  return k_ra8_ok;
}

/* epub_get_resource answers the four-byte stub payload. */
static const uint8_t s_payload[4] = {0xDE, 0xAD, 0xBE, 0xEF};

ra8_err_t epub_get_resource(void *book, const char *path, uint8_t *out_buf, size_t max_len, size_t *got_len)
{
  (void)book;
  (void)path;
  size_t n = (max_len < sizeof(s_payload)) ? max_len : sizeof(s_payload);
  ts_memcpy(out_buf, s_payload, n);
  *got_len = n;
  return k_ra8_ok;
}

/* jof_parse answers fixed 2x2 64x64-tile geometry for every parse. */
ra8_err_t jof_parse(jof_pread_fn pread, void *pread_ctx, uint64_t total_size, jof_info_t *out_info)
{
  (void)pread;
  (void)pread_ctx;
  (void)total_size;
  ts_memset(out_info, 0, sizeof(*out_info));
  out_info->width      = 128;
  out_info->height     = 128;
  out_info->tile_w     = 64;
  out_info->tile_h     = 64;
  out_info->tile_cols  = 2;
  out_info->tile_rows  = 2;
  out_info->bpp        = 4;
  out_info->codec      = 0;
  out_info->tile_count = 4;
  out_info->total_size = 64;
  return k_ra8_ok;
}

/* jof_read_tile: fills the cell with 0xA5 and reports 64x64. A test-set
 * forced error code passes through; 0x105 specifically exercises the
 * binder's invalid_size -> no_mem mapping. */
static ra8_err_t s_forced_tile_err = 0;

void tiles_teststub_set_tile_err(uint16_t code) { s_forced_tile_err = (ra8_err_t)code; }

ra8_err_t jof_read_tile(jof_pread_fn pread, void *pread_ctx, const jof_info_t *info, uint16_t tile_x,
                        uint16_t tile_y, uint8_t *scratch, uint32_t scratch_cap, uint8_t *out_px,
                        uint32_t out_cap, uint16_t *out_w, uint16_t *out_h)
{
  (void)pread;
  (void)pread_ctx;
  (void)info;
  (void)tile_y;
  (void)scratch;
  (void)scratch_cap;
  if (s_forced_tile_err != 0) {
    return s_forced_tile_err;
  }
  uint32_t n = (out_cap < 4096) ? out_cap : 4096;
  ts_memset(out_px, 0xA5, n);
  *out_w = 64;
  *out_h = 64;
  (void)tile_x;
  return k_ra8_ok;
}

/* Tile-cache quartet: init validates the geometry and copies the config;
 * get drives the config's decode trampoline directly (one-cell cache). */
static ra8_tile_cache_cfg_t s_cfg;

ra8_err_t ra8_tile_cache_init(ra8_tile_cache_t *tc, const ra8_tile_cache_cfg_t *cfg)
{
  if ((cfg->cell_bytes == 0) || (cfg->cell_count == 0) || (cfg->bucket_count == 0)) {
    return k_ra8_err_invalid_size;
  }
  ts_memset(tc, 0, sizeof(*tc));
  s_cfg = *cfg;
  return k_ra8_ok;
}

ra8_err_t ra8_tile_cache_get(ra8_tile_cache_t *tc, const ra8_tile_key_t *key, ra8_tile_t *out_tile)
{
  (void)tc;
  uint16_t w = 0;
  uint16_t h = 0;
  ra8_err_t err = s_cfg.decode(s_cfg.decode_ctx, key, s_cfg.cell_mem, s_cfg.cell_bytes, &w, &h);
  if (err != k_ra8_ok) {
    return err;
  }
  out_tile->pixels = s_cfg.cell_mem;
  out_tile->width  = w;
  out_tile->height = h;
  return k_ra8_ok;
}

ra8_err_t ra8_tile_cache_put(ra8_tile_cache_t *tc, const uint8_t *pixels)
{
  (void)tc;
  (void)pixels;
  return k_ra8_ok;
}

ra8_err_t ra8_tile_cache_prefetch_pan(ra8_tile_cache_t *tc, const void *req, uint16_t *out_warmed)
{
  (void)tc;
  (void)req;
  *out_warmed = 1;
  return k_ra8_ok;
}

/* An external-seam pread for add_ext tests. */
ra8_err_t tiles_teststub_ext_pread(void *ctx, uint64_t offset, uint8_t *buf, size_t len, size_t *got)
{
  return epub_entry_pread(NULL, "stub", offset, buf, len, got);
  (void)ctx;
}
