/**
 * @file epub_fs.c
 * @brief `ra8_fs` -> `epub` bridge implementation (Phase 4.2 file adapter).
 *
 * @par Tag
 * [Ring 4 / EPUB] {World: S}
 *
 * @details
 * Opens a `.epub` living on a mounted `ra8_fs` volume by streaming it (#151/#230):
 * the file stays open for the book's lifetime and every ZIP read seeks+reads on
 * demand, so no whole-file buffer ever exists. Guarded on
 * `__has_include("ra8_fs.h")` so the pure `epub` core still links into
 * apps/hosts that do not pull in `ra8_fs` (this TU is then empty -- only the
 * in-memory `epub_open()` path is used).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

/* Bridge only when the ra8_fs storage layer is on the include path (firmware that
 * mounts a filesystem, and the host test build). Otherwise this TU is empty. */
#if __has_include("ra8_fs.h")

#include "epub_fs.h"

#include <stdint.h>

#include "epub_fs_internal.h"
#include "ra8_attributes.h"

RA8_PRIV size_t priv_epub_fs_stream_read(void* ctx, uint64_t offset, void* buf, size_t len)
{
  epub_stream_fs_ctx_t* io = (epub_stream_fs_ctx_t*)ctx;
  if ((io == nullptr) || (io->file == nullptr) || (buf == nullptr)) {
    return 0U;
  }
  if (offset > (uint64_t)UINT32_MAX) {
    return 0U; /* GCOVR_EXCL_LINE -- ra8_fs is 32-bit; a >4 GiB .epub is rejected before here */
  }
  if (ra8_fs_seek(io->file, (uint32_t)offset) != k_ra8_ok) {
    return 0U; /* GCOVR_EXCL_LINE -- seek clamps to size and cannot fail on a live handle */
  }
  /* GCOVR_EXCL_BR_START -- len is a bounded miniz IO chunk, never > 4 GiB */
  const uint32_t want = (len > (size_t)UINT32_MAX) ? UINT32_MAX : (uint32_t)len;
  /* GCOVR_EXCL_BR_STOP */
  uint32_t got = 0U;
  if (ra8_fs_read(io->file, (uint8_t*)buf, want, &got) != k_ra8_ok) {
    return 0U;
  }
  return (size_t)got;
}

[[nodiscard]] ra8_err_t epub_open_streamed_fs(ra8_fs_mount_t*       mount,
                                              const char*           path,
                                              epub_stream_fs_ctx_t* io,
                                              epub_book_t*          out_book)
{
  if ((mount == nullptr) || (path == nullptr) || (io == nullptr) || (out_book == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  io->file = nullptr;

  ra8_fs_file_t* file = nullptr;
  ra8_err_t      err  = ra8_fs_open(mount, path, k_ra8_fs_mode_read, &file);
  if (err != k_ra8_ok) {
    return err;
  }

  /* Size the archive for miniz. ra8_fs_size cannot fail on the handle ra8_fs_open
   * just returned (in_use set, both out-params non-NULL); on any future contract
   * change `size` stays 0 and epub_open_streamed rejects it as invalid_arg,
   * so no separate (untestable) error branch is needed here. */
  uint64_t size64 = 0U;
  (void)ra8_fs_size(file, &size64);
  /* The EPUB pipeline sizes archives in 32 bits; an over-4-GiB file is not a
   * plausible EPUB and is clamped to the reject path (0 -> invalid_arg). */
  const uint32_t size = (size64 <= (uint64_t)UINT32_MAX) ? (uint32_t)size64 : 0U;

  io->file                        = file;
  const epub_stream_media_t media = {
    .read = priv_epub_fs_stream_read,
    .ctx  = io,
    .size = (uint64_t)size,
  };
  err = epub_open_streamed(&media, path, out_book);
  if (err != k_ra8_ok) {
    (void)ra8_fs_close(file);
    io->file = nullptr;
    return err;
  }
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t epub_close_streamed_fs(epub_stream_fs_ctx_t* io, epub_book_t* book)
{
  if ((io == nullptr) || (book == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  const ra8_err_t err = epub_close(book);
  if (io->file != nullptr) {
    (void)ra8_fs_close(io->file);
    io->file = nullptr;
  }
  return err;
}

#endif /* __has_include("ra8_fs.h") */
