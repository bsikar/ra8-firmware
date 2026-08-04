/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file epub_probe.c
 * @brief Host EPUB-probe harness -- report what the ra8_epub pipeline extracts.
 *
 * @details
 * Opens a real `.epub` through `ra8_epub_open()` on the host and prints the
 * parse result, spine length, TOC kind/length and cover path, then loads the
 * first chapter so the miniz inflate + XHTML path is exercised too. Built and
 * run by `tests/fixtures/epub/run_probe.sh`.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "ra8_epub.h"
#include "ra8_err.h"

/**
 * @enum epub_probe_const_t
 * @brief Sizes, indices and exit codes used by the probe.
 *
 * @invariant k_probe_chapter_buf_bytes is large enough for any chapter the
 *            ra8_epub reader hands back on the host.
 *
 * @see probe_slurp()
 */
typedef enum : uint32_t {
  k_probe_exit_ok           = 0U,      /**< Probe completed.                  */
  k_probe_exit_usage        = 2U,      /**< Bad arguments or unreadable file. */
  k_probe_min_argc          = 2U,      /**< argv[0] plus the .epub path.      */
  k_probe_arg_path          = 1U,      /**< Index of the .epub path in argv.  */
  k_probe_first_chapter     = 0U,      /**< Spine index of the first chapter. */
  k_probe_bytes_per_kib     = 1024U,   /**< Divisor for the KiB size report.  */
  k_probe_chapter_buf_bytes = 262144U, /**< 256 KiB chapter staging buffer.   */
} epub_probe_const_t;

/**
 * @brief Log sink stub so the probe does not drag in the ra8_log/ra8_core tree.
 *
 * @param[in] tag Log tag (ignored).
 * @param[in] msg Log message (ignored).
 *
 * @pre Linked in place of the real ra8_log backend.
 * @post Nothing is emitted.
 */
void internal_ra8_log_error(const char* tag, const char* msg)
{
  (void)tag;
  (void)msg;
}

/**
 * @brief Log sink stub for info-level messages.
 *
 * @param[in] tag Log tag (ignored).
 * @param[in] msg Log message (ignored).
 *
 * @pre Linked in place of the real ra8_log backend.
 * @post Nothing is emitted.
 */
void internal_ra8_log_info(const char* tag, const char* msg)
{
  (void)tag;
  (void)msg;
}

/**
 * @brief Log sink stub for warning-level messages.
 *
 * @param[in] tag Log tag (ignored).
 * @param[in] msg Log message (ignored).
 *
 * @pre Linked in place of the real ra8_log backend.
 * @post Nothing is emitted.
 */
void internal_ra8_log_warn(const char* tag, const char* msg)
{
  (void)tag;
  (void)msg;
}

/**
 * @brief Read a whole file into a freshly allocated buffer.
 *
 * @param[in]  path     Path of the file to slurp.
 * @param[out] out_size Receives the byte count on success.
 *
 * @return Malloc'd buffer the caller must free, or nullptr on any failure.
 *
 * @pre @p path is a non-null NUL-terminated string.
 * @pre @p out_size is non-null.
 * @post On success the returned buffer holds exactly `*out_size` bytes.
 * @post On failure the stream and any partial allocation have been released.
 *
 * @note Host-only helper; the firmware itself never allocates (NASA Rule 3).
 */
static uint8_t* probe_slurp(const char* path, size_t* out_size)
{
  FILE* stream = fopen(path, "rb");
  if (stream == nullptr) {
    (void)fprintf(stderr, "cannot open %s\n", path);
    return nullptr;
  }
  if (fseek(stream, 0, SEEK_END) != 0) {
    (void)fclose(stream);
    return nullptr;
  }
  const long size = ftell(stream);
  if (size < 0 || fseek(stream, 0, SEEK_SET) != 0) {
    (void)fclose(stream);
    return nullptr;
  }
  uint8_t* buf = malloc((size_t)size);
  if (buf == nullptr) {
    (void)fclose(stream);
    return nullptr;
  }
  if (fread(buf, 1U, (size_t)size, stream) != (size_t)size) {
    (void)fprintf(stderr, "short read\n");
    free(buf);
    (void)fclose(stream);
    return nullptr;
  }
  (void)fclose(stream);
  *out_size = (size_t)size;
  return buf;
}

/**
 * @brief Print everything ra8_epub_open() extracted from the book.
 *
 * @param[in] path Path the book was read from (echoed in the report).
 * @param[in] size Book size in bytes.
 * @param[in] err  Result of ra8_epub_open().
 * @param[in] book Parsed book descriptor.
 *
 * @pre @p path is a non-null NUL-terminated string.
 * @pre @p book is non-null.
 * @post One report block has been written to stdout.
 * @post @p book is not modified.
 */
static void probe_report(const char* path, size_t size, ra8_err_t err, const ra8_epub_book_t* book)
{
  (void)printf("file=%s  size=%zu KB\n", path, size / (size_t)k_probe_bytes_per_kib);
  (void)printf("  ra8_epub_open -> err=%d (%s)\n", (int)err, (err == k_ra8_ok) ? "OK" : "non-OK");
  (void)printf("  chapter_count = %u  (limit %d)\n",
               (unsigned)book->chapter_count,
               (int)k_ra8_epub_max_chapters);
  (void)printf("  toc_kind = %u (0=none 1=ncx 2=nav)  toc_count = %u (limit %d)\n",
               (unsigned)book->toc_kind,
               (unsigned)book->toc_count,
               (int)k_ra8_epub_max_toc);
  (void)printf("  cover_path = '%s'\n", book->cover_path);
  (void)printf("  opf_dir = '%s'  toc_path = '%s'  toc_kind=%u\n",
               book->opf_dir,
               book->toc_path,
               (unsigned)book->toc_kind);
}

/**
 * @brief Probe entry point.
 *
 * @param[in] argc Argument count.
 * @param[in] argv Argument vector; argv[1] is the .epub path.
 *
 * @return k_probe_exit_ok on success, k_probe_exit_usage on bad input.
 *
 * @pre @p argv is non-null.
 * @pre The named file is a readable .epub.
 * @post The report has been written to stdout.
 * @post Every allocation and stream opened here has been released.
 */
int main(int argc, char** argv)
{
  if (argc < (int)k_probe_min_argc) {
    (void)fprintf(stderr, "usage: epub_probe <file.epub>\n");
    return (int)k_probe_exit_usage;
  }
  const char* path = argv[k_probe_arg_path];
  size_t      size = 0U;
  uint8_t*    buf  = probe_slurp(path, &size);
  if (buf == nullptr) {
    return (int)k_probe_exit_usage;
  }

  ra8_epub_mem_media_t media = {.data = buf, .size = size};
  ra8_epub_book_t      book  = {};
  const ra8_err_t      err   = ra8_epub_open(&media, nullptr, &book);
  probe_report(path, size, err, &book);

  /* Load the first chapter so miniz inflate + xhtml are exercised too. */
  if (err == k_ra8_ok && book.chapter_count > 0U) {
    static uint8_t  s_chbuf[k_probe_chapter_buf_bytes];
    size_t          chlen = 0U;
    const ra8_err_t cerr  = ra8_epub_load_chapter(&book,
                                                  (uint16_t)k_probe_first_chapter,
                                                  s_chbuf,
                                                  sizeof(s_chbuf),
                                                  &chlen);
    (void)printf("  load_chapter(0) -> err=%d  bytes=%zu\n", (int)cerr, chlen);
  }
  free(buf);
  return (int)k_probe_exit_ok;
}
