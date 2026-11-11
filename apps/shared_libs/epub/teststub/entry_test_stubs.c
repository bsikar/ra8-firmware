/* Test-only stand-ins for the RA8FW-1165 entry TU's externs: the miniz
 * reader/iterator trio, the zip guard, and the sibling-archive priv_* helpers.
 * Referenced only by build.zig; production links the real ones. */

#include <stddef.h>
#include <stdint.h>

typedef uint16_t ra8_err_t;

typedef struct { unsigned char storage[64]; } mz_zip_reader_extract_iter_state;

int teststub_locate_idx = -1;
int teststub_miss_prefixed = 0;
int teststub_locate_calls = 0;
int teststub_fail_stat = 0;
uint16_t teststub_stat_method = 0;
uint64_t teststub_stat_uncomp = 0;
uint64_t teststub_stat_comp = 0;
uint64_t teststub_stat_lho = 0;
int teststub_guard_result = 0;
int teststub_not_ready = 0;
int teststub_fail_iter = 0;
size_t teststub_iter_remaining = 0;
uint8_t teststub_iter_fill = 0xA5;
int teststub_iter_free_result = 1;

/* struct mz_zip_archive and mz_zip_archive_file_stat are opaque here; the
 * stub writes through the full C layout the TU mirrors. */
void *mz_zip_reader_locate_file_zip_dummy;

int mz_zip_reader_locate_file(void *zip, const char *name, const char *comment, unsigned flags) {
  (void)zip; (void)comment; (void)flags;
  teststub_locate_calls++;
  if (teststub_miss_prefixed && teststub_locate_calls == 1) return -1;
  return teststub_locate_idx;
}

int mz_zip_reader_file_stat(void *zip, unsigned index, void *pStat) {
  (void)zip; (void)index;
  if (teststub_fail_stat) return 0;
  /* Write through the mirrored field offsets (64-bit layout, RA8FW-1165 probe):
   * method @22, comp @32, uncomp @40, local_header_ofs @56. */
  unsigned char *s = (unsigned char *)pStat;
  uint16_t method = teststub_stat_method;
  uint64_t comp = teststub_stat_comp;
  uint64_t uncomp = teststub_stat_uncomp;
  uint64_t lho = teststub_stat_lho;
  __builtin_memcpy(s + 22, &method, sizeof(method));
  __builtin_memcpy(s + 32, &comp, sizeof(comp));
  __builtin_memcpy(s + 40, &uncomp, sizeof(uncomp));
  __builtin_memcpy(s + 56, &lho, sizeof(lho));
  return 1;
}

ra8_err_t priv_epub_zip_guard_entry(const void *pStat) {
  (void)pStat;
  return (ra8_err_t)teststub_guard_result;
}

int priv_epub_book_not_ready(uint8_t in_use, uint8_t zip_archive_active) {
  (void)in_use; (void)zip_archive_active;
  return teststub_not_ready;
}

void priv_epub_join_path(const char *dir, const char *name, char *dst, size_t cap) {
  size_t used = 0;
  for (const char *p = dir ? dir : ""; *p && used + 1 < cap; p++) dst[used++] = *p;
  for (const char *p = name ? name : ""; *p && used + 1 < cap; p++) dst[used++] = *p;
  if (cap) dst[used] = 0;
}

static mz_zip_reader_extract_iter_state iter_storage;

mz_zip_reader_extract_iter_state *mz_zip_reader_extract_iter_new(void *zip, unsigned index, unsigned flags) {
  (void)zip; (void)index; (void)flags;
  if (teststub_fail_iter) return (mz_zip_reader_extract_iter_state *)0;
  return &iter_storage;
}

size_t mz_zip_reader_extract_iter_read(mz_zip_reader_extract_iter_state *st, uint8_t *buf, size_t cap) {
  (void)st;
  size_t n = cap < teststub_iter_remaining ? cap : teststub_iter_remaining;
  __builtin_memset(buf, (int)teststub_iter_fill, n);
  teststub_iter_remaining -= n;
  return n;
}

int mz_zip_reader_extract_iter_free(mz_zip_reader_extract_iter_state *st) {
  (void)st;
  return teststub_iter_free_result;
}
