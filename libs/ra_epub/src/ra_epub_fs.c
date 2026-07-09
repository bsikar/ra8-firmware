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

#endif /* __has_include("ra_fs.h") */
