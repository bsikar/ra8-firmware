/* Host EPUB-probe harness: open a real .epub via ra8_epub and report what
 * our pipeline extracts (parse result, spine length, TOC, cover). Host-only
 * diagnostic; not committed.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "ra8_epub.h"
#include "ra8_err.h"

/* Logstub so we don't drag the whole ra8_log/ra8_core tree in. */
void internal_ra8_log_error(const char* a, const char* b)
{
  (void)a;
  (void)b;
}
void internal_ra8_log_info(const char* a, const char* b)
{
  (void)a;
  (void)b;
}
void internal_ra8_log_warn(const char* a, const char* b)
{
  (void)a;
  (void)b;
}

int main(int argc, char** argv)
{
  if (argc < 2) {
    fprintf(stderr, "usage: epub_probe <file.epub>\n");
    return 2;
  }
  FILE* f = fopen(argv[1], "rb");
  if (!f) {
    fprintf(stderr, "cannot open %s\n", argv[1]);
    return 2;
  }
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t* buf = (uint8_t*)malloc((size_t)sz);
  if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
    fprintf(stderr, "short read\n");
    return 2;
  }
  fclose(f);

  ra8_epub_mem_media_t media = {.data = buf, .size = (size_t)sz};
  ra8_epub_book_t      book  = {};
  ra8_err_t            err   = ra8_epub_open(&media, NULL, &book);

  printf("file=%s  size=%ld KB\n", argv[1], sz / 1024);
  printf("  ra8_epub_open -> err=%d (%s)\n", (int)err, err == k_ra8_ok ? "OK" : "non-OK");
  printf("  chapter_count = %u  (limit %d)\n",
         (unsigned)book.chapter_count,
         (int)k_ra8_epub_max_chapters);
  printf("  toc_kind = %u (0=none 1=ncx 2=nav)  toc_count = %u (limit %d)\n",
         (unsigned)book.toc_kind,
         (unsigned)book.toc_count,
         (int)k_ra8_epub_max_toc);
  printf("  cover_path = '%s'\n", book.cover_path);
  printf("  opf_dir = '%s'  toc_path = '%s'  toc_kind=%u\n",
         book.opf_dir,
         book.toc_path,
         (unsigned)book.toc_kind);

  /* Try to load the first chapter so we exercise miniz inflate + xhtml. */
  if (err == k_ra8_ok && book.chapter_count > 0) {
    static uint8_t s_chbuf[256 * 1024];
    size_t         chlen = 0;
    ra8_err_t      cerr  = ra8_epub_load_chapter(&book, 0, s_chbuf, sizeof(s_chbuf), &chlen);
    printf("  load_chapter(0) -> err=%d  bytes=%zu\n", (int)cerr, chlen);
  }
  free(buf);
  return 0;
}
