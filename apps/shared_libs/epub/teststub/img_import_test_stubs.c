/* Test-only stand-ins for the RA8FW-1193 img-import TU's externs: the entry
 * cursor quartet, the tile-binder registration pair and jof_produce.
 * Referenced only by build.zig; production links the real ones. */

#include <stddef.h>
#include <stdint.h>

typedef uint16_t ra8_err_t;
typedef struct epub_book epub_book_t;
typedef struct epub_tile_binder epub_tile_binder_t;
typedef uint16_t (*jof_sink_fn)(void*, const uint8_t*, size_t);
typedef uint16_t (*jof_pread_fn)(void*, uint64_t, uint8_t*, size_t, size_t*);
typedef uint16_t (*jof_pull_fn)(void*, uint8_t*, size_t, size_t*);

typedef struct {
  void* iter;
  epub_book_t* book;
  uint64_t total;
  uint64_t consumed;
  uint8_t done;
  uint8_t reserved[7];
} epub_entry_reader_t;

typedef struct {
  uint16_t width, height, tile_w, tile_h, tile_cols, tile_rows;
  uint8_t bpp, codec;
  uint32_t tile_count, index_off, total_size;
} jof_info_t;

typedef struct {
  jof_pull_fn pull;
  void* pull_ctx;
  jof_sink_fn sink;
  void* sink_ctx;
  uint16_t tile_w, tile_h;
  uint8_t codec;
  uint16_t max_width, max_height;
  uint8_t* work;
  size_t work_cap;
  uint8_t* webp_work;
  size_t webp_work_cap;
} jof_produce_cfg_t;

int teststub_open_calls = 0;
int teststub_read_calls = 0;
int teststub_close_calls = 0;
int teststub_pread_calls = 0;
int teststub_add_calls = 0;
int teststub_add_ext_calls = 0;
int teststub_produce_calls = 0;
uint16_t teststub_open_result = 0;
uint16_t teststub_close_result = 0;
uint16_t teststub_pread_result = 0;
uint16_t teststub_add_result = 0;
uint16_t teststub_add_ext_result = 0;
uint16_t teststub_produce_result = 0;
uint64_t teststub_entry_size = 0;
size_t teststub_read_remaining = 0;
uint8_t teststub_read_fill = 0xA5;
size_t teststub_read_cap_seen = 0;
uint32_t teststub_produced_total = 0;
uint32_t teststub_add_image_id = 0;
char teststub_add_path[256] = {0};
uint64_t teststub_add_ext_total = 0;
uint32_t teststub_add_ext_image_id = 0;
int teststub_add_ext_pread_seen = 0;
int teststub_add_ext_ctx_seen = 0;
uint16_t teststub_pcfg_tile_w = 0;
uint16_t teststub_pcfg_tile_h = 0;
uint8_t teststub_pcfg_codec = 0;
uint16_t teststub_pcfg_max_w = 0;
uint16_t teststub_pcfg_max_h = 0;
size_t teststub_pcfg_work_cap = 0;
size_t teststub_pcfg_webp_cap = 0;
int teststub_pcfg_work_seen = 0;
int teststub_pcfg_webp_seen = 0;
int teststub_pcfg_sink_seen = 0;
int teststub_pcfg_sink_ctx_seen = 0;
int teststub_pcfg_pull_ctx_seen = 0;
int teststub_pcfg_pull_seen = 0;
uint8_t teststub_pread_buf[4] = {0};
size_t teststub_pread_len = 0;

static void ts_memset(void* dst, uint8_t value, size_t n) {
  uint8_t* d = (uint8_t*)dst;
  for (size_t i = 0; i < n; i++) d[i] = value;
}

static void ts_memcpy(void* dst, const void* src, size_t n) {
  uint8_t* d = (uint8_t*)dst;
  const uint8_t* s = (const uint8_t*)src;
  for (size_t i = 0; i < n; i++) d[i] = s[i];
}

void teststub_reset(void) {
  teststub_open_calls = 0;
  teststub_read_calls = 0;
  teststub_close_calls = 0;
  teststub_pread_calls = 0;
  teststub_add_calls = 0;
  teststub_add_ext_calls = 0;
  teststub_produce_calls = 0;
  teststub_open_result = 0;
  teststub_close_result = 0;
  teststub_pread_result = 0;
  teststub_add_result = 0;
  teststub_add_ext_result = 0;
  teststub_produce_result = 0;
  teststub_entry_size = 0;
  teststub_read_remaining = 0;
  teststub_read_fill = 0xA5;
  teststub_read_cap_seen = 0;
  teststub_produced_total = 0;
  teststub_add_image_id = 0;
  ts_memset(teststub_add_path, 0, sizeof(teststub_add_path));
  teststub_add_ext_total = 0;
  teststub_add_ext_image_id = 0;
  teststub_add_ext_pread_seen = 0;
  teststub_add_ext_ctx_seen = 0;
  teststub_pcfg_tile_w = 0;
  teststub_pcfg_tile_h = 0;
  teststub_pcfg_codec = 0;
  teststub_pcfg_max_w = 0;
  teststub_pcfg_max_h = 0;
  teststub_pcfg_work_cap = 0;
  teststub_pcfg_webp_cap = 0;
  teststub_pcfg_work_seen = 0;
  teststub_pcfg_webp_seen = 0;
  teststub_pcfg_sink_seen = 0;
  teststub_pcfg_sink_ctx_seen = 0;
  teststub_pcfg_pull_ctx_seen = 0;
  teststub_pcfg_pull_seen = 0;
  ts_memset(teststub_pread_buf, 0, sizeof(teststub_pread_buf));
  teststub_pread_len = 0;
}

ra8_err_t epub_entry_open(epub_book_t* book, const char* path, epub_entry_reader_t* out_reader, uint64_t* out_size) {
  (void)path;
  teststub_open_calls++;
  ts_memset(out_reader, 0, sizeof(*out_reader));
  out_reader->iter = (void*)1;
  out_reader->book = book;
  out_reader->total = teststub_entry_size;
  if (out_size != 0) *out_size = teststub_entry_size;
  return teststub_open_result;
}

ra8_err_t epub_entry_read(epub_entry_reader_t* reader, uint8_t* buf, size_t cap, size_t* got) {
  teststub_read_calls++;
  teststub_read_cap_seen = cap;
  size_t n = cap < teststub_read_remaining ? cap : teststub_read_remaining;
  if (buf != 0) ts_memset(buf, teststub_read_fill, n);
  teststub_read_remaining -= n;
  if (got != 0) *got = n;
  if (teststub_read_remaining == 0 && reader != 0) reader->done = 1;
  return 0;
}

ra8_err_t epub_entry_close(epub_entry_reader_t* reader) {
  teststub_close_calls++;
  if (reader != 0) reader->iter = 0;
  return teststub_close_result;
}

ra8_err_t epub_entry_pread(epub_book_t* book, const char* path, uint64_t offset, uint8_t* buf, size_t len, size_t* got) {
  (void)book;
  (void)path;
  (void)offset;
  teststub_pread_calls++;
  if (got != 0) *got = 0;
  if (teststub_pread_result != 0) return teststub_pread_result;
  size_t n = len < teststub_pread_len ? len : teststub_pread_len;
  if (buf != 0 && n != 0) ts_memcpy(buf, teststub_pread_buf, n);
  if (got != 0) *got = n;
  return 0;
}

ra8_err_t epub_tile_binder_add(epub_tile_binder_t* binder, epub_book_t* book, const char* path, uint32_t image_id) {
  (void)binder;
  (void)book;
  teststub_add_calls++;
  teststub_add_image_id = image_id;
  const char* src = path ? path : "";
  size_t j = 0;
  for (; src[j] != 0 && j < sizeof(teststub_add_path) - 1; j++) teststub_add_path[j] = src[j];
  for (; j < sizeof(teststub_add_path); j++) teststub_add_path[j] = 0;
  return teststub_add_result;
}

ra8_err_t epub_tile_binder_add_ext(epub_tile_binder_t* binder, jof_pread_fn pread, void* pread_ctx, uint64_t total_size, uint32_t image_id) {
  (void)binder;
  teststub_add_ext_calls++;
  teststub_add_ext_total = total_size;
  teststub_add_ext_image_id = image_id;
  teststub_add_ext_pread_seen = pread != 0;
  teststub_add_ext_ctx_seen = pread_ctx != 0;
  return teststub_add_ext_result;
}

ra8_err_t jof_produce(const jof_produce_cfg_t* cfg, jof_info_t* out_info) {
  teststub_produce_calls++;
  if (cfg != 0) {
    teststub_pcfg_tile_w = cfg->tile_w;
    teststub_pcfg_tile_h = cfg->tile_h;
    teststub_pcfg_codec = cfg->codec;
    teststub_pcfg_max_w = cfg->max_width;
    teststub_pcfg_max_h = cfg->max_height;
    teststub_pcfg_work_cap = cfg->work_cap;
    teststub_pcfg_webp_cap = cfg->webp_work_cap;
    teststub_pcfg_work_seen = cfg->work != 0;
    teststub_pcfg_webp_seen = cfg->webp_work != 0;
    teststub_pcfg_sink_seen = cfg->sink != 0;
    teststub_pcfg_sink_ctx_seen = cfg->sink_ctx != 0;
    teststub_pcfg_pull_ctx_seen = cfg->pull_ctx != 0;
    teststub_pcfg_pull_seen = cfg->pull != 0;
  }
  if (out_info != 0) {
    ts_memset(out_info, 0, sizeof(*out_info));
    out_info->total_size = teststub_produced_total;
  }
  return teststub_produce_result;
}
