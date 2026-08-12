/**
 * @file ra8_epub_fs.c
 * @brief `ra8_fs` -> `ra8_epub` bridge implementation (Phase 4.2 file adapter).
 *
 * @par Tag
 * [Ring 4 / EPUB] {World: S}
 *
 * @details
 * Opens a `.epub` living on a mounted `ra8_fs` volume by streaming it (#151/#230):
 * the file stays open for the book's lifetime and every ZIP read seeks+reads on
 * demand, so no whole-file buffer ever exists. Guarded on
 * `__has_include("ra8_fs.h")` so the pure `ra8_epub` core still links into
 * apps/hosts that do not pull in `ra8_fs` (this TU is then empty -- only the
 * in-memory `ra8_epub_open()` path is used).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

/* Bridge only when the ra8_fs storage layer is on the include path (firmware that
 * mounts a filesystem, and the host test build). Otherwise this TU is empty. */
#if __has_include("ra8_fs.h")

#include "ra8_epub_fs.h"

#include <stdint.h>

#include "ra8_attributes.h"

/**
 * @brief ra8_epub stream read callback backed by an open `ra8_fs` file (#151).
 *
 * @details
 * Seeks to @p offset and reads up to @p len bytes. `ra8_fs` uses 32-bit file
 * offsets, so an offset beyond 4 GiB (a single `.epub` larger than `ra8_fs` can
 * address) short-circuits to a 0-byte read, which miniz treats as a read error.
 *
 * @param[in]  ctx    The ::ra8_epub_stream_fs_ctx_t holding the open file.
 * @param[in]  offset Absolute byte offset within the `.epub`.
 * @param[out] buf    Destination buffer (`len` writable bytes).
 * @param[in]  len    Bytes requested.
 * @return Bytes actually read (0 on any error / EOF).
 * @retval len  The full request was read from the file.
 * @retval 0    Error, EOF, or @p offset beyond the 32-bit `ra8_fs` addressable range.
 * @retval <len A short read near EOF; the bytes read before the file end.
 * @pre @p ctx holds a live open file; @p buf is writable for @p len bytes.
 * @pre @p offset addresses the source file.
 * @post No state outside @p buf and the file offset is modified.
 * @post At most @p len bytes are written to @p buf.
 * @note Not thread-safe; single-threaded reader context.
 * @since 0.1.0
 */
RA8_INTERNAL
static size_t priv_fs_stream_read(void* ctx, uint64_t offset, void* buf, size_t len)
{
  ra8_epub_stream_fs_ctx_t* io = (ra8_epub_stream_fs_ctx_t*)ctx;
  if (io == nullptr || io->file == nullptr || buf == nullptr) {
    return 0U; /* GCOVR_EXCL_LINE -- open binds a live ctx+file; miniz never reads a NULL buf */
  }
  if (offset > (uint64_t)UINT32_MAX) {
    return 0U; /* GCOVR_EXCL_LINE -- ra8_fs is 32-bit; a >4 GiB .epub is rejected before here */
  }
  if (ra8_fs_seek(io->file, (uint32_t)offset) != k_ra8_ok) {
    return 0U; /* GCOVR_EXCL_LINE -- seek clamps to size and cannot fail on a live handle */
  }
  const uint32_t want =
    (len > (size_t)UINT32_MAX)
      ? UINT32_MAX
      : (uint32_t)len; /* GCOVR_EXCL_BR_LINE -- len is a bounded miniz IO chunk, never > 4 GiB */
  uint32_t got = 0U;
  if (ra8_fs_read(io->file, (uint8_t*)buf, want, &got) != k_ra8_ok) {
    return 0U; /* GCOVR_EXCL_LINE -- read cannot fail on a live read-mode handle */
  }
  return (size_t)got;
}

[[nodiscard]] ra8_err_t ra8_epub_open_streamed_fs(ra8_fs_mount_t*           mount,
                                                  const char*               path,
                                                  ra8_epub_stream_fs_ctx_t* io,
                                                  ra8_epub_book_t*          out_book)
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
   * change `size` stays 0 and ra8_epub_open_streamed rejects it as invalid_arg,
   * so no separate (untestable) error branch is needed here. */
  uint64_t size64 = 0U;
  (void)ra8_fs_size(file, &size64);
  /* The EPUB pipeline sizes archives in 32 bits; an over-4-GiB file is not a
   * plausible EPUB and is clamped to the reject path (0 -> invalid_arg). */
  const uint32_t size = (size64 <= (uint64_t)UINT32_MAX) ? (uint32_t)size64 : 0U;

  io->file                            = file;
  const ra8_epub_stream_media_t media = {
    .read = priv_fs_stream_read,
    .ctx  = io,
    .size = (uint64_t)size,
  };
  err = ra8_epub_open_streamed(&media, path, out_book);
  if (err != k_ra8_ok) {
    (void)ra8_fs_close(file);
    io->file = nullptr;
    return err;
  }
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_epub_close_streamed_fs(ra8_epub_stream_fs_ctx_t* io,
                                                   ra8_epub_book_t*          book)
{
  if (io == nullptr || book == nullptr) {
    return k_ra8_err_null_ptr;
  }
  const ra8_err_t err = ra8_epub_close(book);
  if (io->file != nullptr) {
    (void)ra8_fs_close(io->file);
    io->file = nullptr;
  }
  return err;
}

#endif /* __has_include("ra8_fs.h") */
