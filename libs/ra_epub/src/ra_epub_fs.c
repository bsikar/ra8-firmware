/**
 * @file ra_epub_fs.c
 * @brief `ra_fs` -> `ra_epub` bridge implementation (Phase 4.2 file adapter).
 *
 * @par Tag
 * [Ring 4 / EPUB] {World: S}
 *
 * @details
 * Reads a whole `.epub` off a mounted `ra_fs` volume into a caller buffer, then
 * opens it with `ra_epub_open()`. Guarded on `__has_include("ra_fs.h")` so the
 * pure `ra_epub` core still links into apps/hosts that do not pull in `ra_fs`
 * (this TU is then empty -- only the in-memory `ra_epub_open()` path is used).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

/* Bridge only when the ra_fs storage layer is on the include path (firmware that
 * mounts a filesystem, and the host test build). Otherwise this TU is empty. */
#if __has_include("ra_fs.h")

#include "ra_epub_fs.h"

#include <stdint.h>

[[nodiscard]] ra_err_t ra_epub_open_fs(ra_fs_mount_t*  mount,
                                       const char*     path,
                                       uint8_t*        buf,
                                       size_t          cap,
                                       ra_epub_book_t* out_book)
{
  if ((mount == nullptr) || (path == nullptr) || (buf == nullptr) || (out_book == nullptr)) {
    return k_ra_err_null_ptr;
  }
  if (cap == 0U) {
    return k_ra_err_invalid_size;
  }

  ra_fs_file_t* file = nullptr;
  ra_err_t      err  = ra_fs_open(mount, path, k_ra_fs_mode_read, &file);
  if (err != k_ra_ok) {
    return err;
  }

  uint32_t size = 0U;
  err           = ra_fs_size(file, &size);
  // mcdc-deactivated: DO-178C 6.4.4.3 -- C1 (`err == k_ra_ok`) is invariantly true here: ra_fs_size() cannot fail on the handle ra_fs_open() just returned (its in_use flag is set and both out-params are non-NULL), so no public-API input flips C1. Only C2 (size > cap) varies, and both its arms are tested (roundtrip: false; buffer-too-small: true). The err guard is defensive against a future ra_fs_size contract change.
  if ((err == k_ra_ok) && ((size_t)size > cap)) {
    err = k_ra_err_no_mem; /* book does not fit the caller buffer */
  }

  uint32_t got = 0U;
  if (err == k_ra_ok) {
    err = ra_fs_read(file, buf, size, &got);
  }
  // mcdc-deactivated: DO-178C 6.4.4.3 -- the outcome-true vector (C1 && C2) requires a successful read (err == k_ra_ok) that returned fewer bytes than requested (got != size). ra_fs_read() reports k_ra_ok only after producing exactly `size` bytes (offset 0, max_len == size), so got == size on every success and (T,T) is unreachable on any public-API path. With no outcome-true vector neither condition can be shown independently; the short-read guard is defensive against a future backend that violates that contract.
  if ((err == k_ra_ok) && (got != size)) {
    err = k_ra_err_hw_error; /* short read -- file shrank or backend hiccup */
  }
  (void)ra_fs_close(file); /* always release the handle */
  if (err != k_ra_ok) {
    return err;
  }

  const ra_epub_mem_media_t media = {.data = buf, .size = (size_t)got};
  return ra_epub_open(&media, path, out_book);
}

/**
 * @brief ra_epub stream read callback backed by an open `ra_fs` file (#151).
 *
 * @details
 * Seeks to @p offset and reads up to @p len bytes. `ra_fs` uses 32-bit file
 * offsets, so an offset beyond 4 GiB (a single `.epub` larger than `ra_fs` can
 * address) short-circuits to a 0-byte read, which miniz treats as a read error.
 *
 * @param[in]  ctx    The ::ra_epub_stream_fs_ctx_t holding the open file.
 * @param[in]  offset Absolute byte offset within the `.epub`.
 * @param[out] buf    Destination buffer (`len` writable bytes).
 * @param[in]  len    Bytes requested.
 * @return Bytes actually read (0 on any error / EOF).
 * @pre @p ctx holds a live open file; @p buf is writable for @p len bytes.
 * @pre @p offset addresses the source file.
 * @post No state outside @p buf and the file offset is modified.
 * @note Not thread-safe; single-threaded reader context.
 * @since 0.1.0
 */
static size_t priv_fs_stream_read(void* ctx, uint64_t offset, void* buf, size_t len)
{
  ra_epub_stream_fs_ctx_t* io = (ra_epub_stream_fs_ctx_t*)ctx;
  if (io == nullptr || io->file == nullptr || buf == nullptr) {
    return 0U; /* GCOVR_EXCL_LINE -- open binds a live ctx+file; miniz never reads a NULL buf */
  }
  if (offset > (uint64_t)UINT32_MAX) {
    return 0U; /* GCOVR_EXCL_LINE -- ra_fs is 32-bit; a > 4 GiB single .epub is rejected before here */
  }
  if (ra_fs_seek(io->file, (uint32_t)offset) != k_ra_ok) {
    return 0U; /* GCOVR_EXCL_LINE -- seek clamps to size and cannot fail on a live handle */
  }
  const uint32_t want =
    (len > (size_t)UINT32_MAX)
      ? UINT32_MAX
      : (uint32_t)len; /* GCOVR_EXCL_BR_LINE -- len is a bounded miniz IO chunk, never > 4 GiB */
  uint32_t got = 0U;
  if (ra_fs_read(io->file, (uint8_t*)buf, want, &got) != k_ra_ok) {
    return 0U; /* GCOVR_EXCL_LINE -- read cannot fail on a live read-mode handle */
  }
  return (size_t)got;
}

[[nodiscard]] ra_err_t ra_epub_open_streamed_fs(ra_fs_mount_t*           mount,
                                                const char*              path,
                                                ra_epub_stream_fs_ctx_t* io,
                                                ra_epub_book_t*          out_book)
{
  if ((mount == nullptr) || (path == nullptr) || (io == nullptr) || (out_book == nullptr)) {
    return k_ra_err_null_ptr;
  }
  io->file = nullptr;

  ra_fs_file_t* file = nullptr;
  ra_err_t      err  = ra_fs_open(mount, path, k_ra_fs_mode_read, &file);
  if (err != k_ra_ok) {
    return err;
  }

  /* Size the archive for miniz. ra_fs_size cannot fail on the handle ra_fs_open
   * just returned (in_use set, both out-params non-NULL); on any future contract
   * change `size` stays 0 and ra_epub_open_streamed rejects it as invalid_arg,
   * so no separate (untestable) error branch is needed here. */
  uint32_t size = 0U;
  (void)ra_fs_size(file, &size);

  io->file                           = file;
  const ra_epub_stream_media_t media = {
    .read = priv_fs_stream_read,
    .ctx  = io,
    .size = (uint64_t)size,
  };
  err = ra_epub_open_streamed(&media, path, out_book);
  if (err != k_ra_ok) {
    (void)ra_fs_close(file);
    io->file = nullptr;
    return err;
  }
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_epub_close_streamed_fs(ra_epub_stream_fs_ctx_t* io, ra_epub_book_t* book)
{
  if (io == nullptr || book == nullptr) {
    return k_ra_err_null_ptr;
  }
  const ra_err_t err = ra_epub_close(book);
  if (io->file != nullptr) {
    (void)ra_fs_close(io->file);
    io->file = nullptr;
  }
  return err;
}

#endif /* __has_include("ra_fs.h") */
