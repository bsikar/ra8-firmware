/**
 * @file epub_probe.c
 * @brief Host EPUB-probe harness over a bounded raw-descriptor source.
 *
 * @details Opens a real `.epub` through `epub_open_streamed()` and reports
 * the parsed spine, TOC, cover, and first-chapter result through an injected
 * text sink. The archive is never copied into a whole-file heap buffer.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>

#include "epub.h"
#include "ra8_err.h"
#include "ra8_test_output.h"

/** @brief Sizes, indices, descriptors, and exit codes used by the probe. */
typedef enum : uint32_t {
  k_probe_exit_ok           = 0U,      /**< Probe completed.                  */
  k_probe_exit_usage        = 2U,      /**< Bad arguments or unreadable file. */
  k_probe_min_argc          = 2U,      /**< argv[0] plus the EPUB path.       */
  k_probe_arg_path          = 1U,      /**< Index of the EPUB path in argv.   */
  k_probe_first_chapter     = 0U,      /**< Spine index of the first chapter. */
  k_probe_bytes_per_kib     = 1024U,   /**< Divisor for the KiB report.       */
  k_probe_chapter_buf_bytes = 262144U, /**< 256 KiB chapter staging buffer.   */
} epub_probe_const_t;

/** @brief Borrowed descriptor backing the streamed EPUB callback. */
typedef struct {
  int descriptor; /**< Open read-only descriptor. */
} epub_probe_source_t;

/** @brief Read one absolute archive span, retrying interrupted host calls. @details Implements the probe read fixture operation used only by this focused test executable. @param[in,out] context Fixture argument governed by the exercised interface contract. @param[in] offset Fixture argument governed by the exercised interface contract. @param[out] destination Fixture argument governed by the exercised interface contract. @param[in] length Fixture argument governed by the exercised interface contract. @return The value computed by the fixture helper. @retval value The computed fixture value for the supplied inputs. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static size_t
internal_probe_read(void* context, uint64_t offset, void* destination, size_t length)
{
  if ((context == nullptr) || ((destination == nullptr) && (length != 0U)) ||
      (offset > (uint64_t)INT64_MAX)) {
    return 0U;
  }
  const epub_probe_source_t* source  = (const epub_probe_source_t*)context;
  size_t                     used    = 0U;
  bool                       reading = true;
  while (reading && (used < length)) {
    if ((offset > (UINT64_MAX - used)) || ((offset + used) > (uint64_t)INT64_MAX)) {
      reading = false;
      continue;
    }
    ssize_t got = -1;
    do {
      errno = 0;
      got   = pread(source->descriptor,
                    &((uint8_t*)destination)[used],
                    length - used,
                    (off_t)(offset + used));
    } while ((got < 0) && (errno == EINTR));
    if (got <= 0) {
      reading = false;
    } else {
      used += (size_t)got;
    }
  }
  return used;
}

/** @brief Append one label and unsigned value to the injected report. @details Implements the probe u64 fixture operation used only by this focused test executable. @param[out] output Fixture argument governed by the exercised interface contract. @param[in] label Fixture argument governed by the exercised interface contract. @param[in] value Fixture argument governed by the exercised interface contract. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void
internal_probe_u64(ra8_test_output_t* output, const char* label, uint64_t value)
{
  (void)internal_test_output_text(output, label);
  (void)internal_test_output_u64(output, value);
  (void)internal_test_output_text(output, "\n");
}

/** @brief Emit the parser's fixed-field summary without formatted I/O. @details Implements the probe report fixture operation used only by this focused test executable. @param[out] output Fixture argument governed by the exercised interface contract. @param[in] path Fixture argument governed by the exercised interface contract. @param[in] size Fixture argument governed by the exercised interface contract. @param[in] error Fixture argument governed by the exercised interface contract. @param[in] book Fixture argument governed by the exercised interface contract. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static void internal_probe_report(ra8_test_output_t* output,
                                               const char*        path,
                                               uint64_t           size,
                                               ra8_err_t          error,
                                               const epub_book_t* book)
{
  (void)internal_test_output_text(output, "file=");
  (void)internal_test_output_text(output, path);
  (void)internal_test_output_text(output, "\nparse_error=");
  (void)internal_test_output_i64(output, (int64_t)error);
  (void)internal_test_output_text(output, "\n");
  internal_probe_u64(output, "size_kib=", size / (uint64_t)k_probe_bytes_per_kib);
  internal_probe_u64(output, "chapter_count=", book->chapter_count);
  internal_probe_u64(output, "toc_kind=", book->toc_kind);
  internal_probe_u64(output, "toc_count=", book->toc_count);
  (void)internal_test_output_text(output, "cover_path=");
  (void)internal_test_output_text(output, book->cover_path);
  (void)internal_test_output_text(output, "\nopf_dir=");
  (void)internal_test_output_text(output, book->opf_dir);
  (void)internal_test_output_text(output, "\ntoc_path=");
  (void)internal_test_output_text(output, book->toc_path);
  (void)internal_test_output_text(output, "\n");
}

/** @brief Open the descriptor-backed book and exercise its first chapter. @details Implements the probe book fixture operation used only by this focused test executable. @param[in] path Fixture argument governed by the exercised interface contract. @param[in] descriptor Fixture argument governed by the exercised interface contract. @param[in] size Fixture argument governed by the exercised interface contract. @param[out] output Fixture argument governed by the exercised interface contract. @return RA8 status from the exercised fixture operation. @retval k_ra8_ok The fixture operation completed successfully. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL static ra8_err_t
internal_probe_book(const char* path, int descriptor, uint64_t size, ra8_test_output_t* output)
{
  epub_probe_source_t source = {.descriptor = descriptor};
  epub_stream_media_t media  = {
    .read = internal_probe_read,
    .ctx  = &source,
    .size = size,
  };
  epub_book_t book  = {};
  ra8_err_t   error = epub_open_streamed(&media, path, &book);
  internal_probe_report(output, path, size, error, &book);
  if ((error == k_ra8_ok) && (book.chapter_count > 0U)) {
    static uint8_t  s_chapter[k_probe_chapter_buf_bytes];
    size_t          chapter_length = 0U;
    const ra8_err_t chapter_error  = epub_load_chapter(&book,
                                                       (uint16_t)k_probe_first_chapter,
                                                       s_chapter,
                                                       sizeof(s_chapter),
                                                       &chapter_length);
    (void)internal_test_output_text(output, "chapter_error=");
    (void)internal_test_output_i64(output, (int64_t)chapter_error);
    (void)internal_test_output_text(output, "\n");
    internal_probe_u64(output, "chapter_bytes=", chapter_length);
    (void)epub_close(&book);
  }
  return error;
}

/** @brief Probe one EPUB path supplied on the command line. */
int main(int argc, char** argv)
{
  ra8_test_output_t    output       = {};
  ra8_test_output_fd_t output_state = {};
  (void)internal_test_output_fd_init(&output, &output_state, STDOUT_FILENO);
  if ((argv == nullptr) || (argc < (int)k_probe_min_argc)) {
    (void)internal_test_output_fd_text(STDERR_FILENO, "usage: epub_probe <file.epub>\n");
    return (int)k_probe_exit_usage;
  }
  const char* path       = argv[k_probe_arg_path];
  const int   descriptor = open(path, O_RDONLY);
  struct stat attributes = {};
  if ((descriptor < 0) || (fstat(descriptor, &attributes) != 0) || (attributes.st_size <= 0)) {
    if (descriptor >= 0) {
      (void)close(descriptor);
    }
    (void)internal_test_output_fd_text(STDERR_FILENO, "cannot open EPUB\n");
    return (int)k_probe_exit_usage;
  }
  (void)internal_probe_book(path, descriptor, (uint64_t)attributes.st_size, &output);
  (void)close(descriptor);
  return (output.status == k_ra8_test_output_ok) ? (int)k_probe_exit_ok : (int)k_probe_exit_usage;
}
