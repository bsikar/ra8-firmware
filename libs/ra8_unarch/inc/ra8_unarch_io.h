/**
 * @file ra8_unarch_io.h
 * @brief Shared seek+read seam and flat-memory backing for the unarchivers.
 * @ingroup grp_ereader
 *
 * @par Tag
 * [Ring 4 / Domain] {World: NS}
 *
 * @details
 * Every decoder in `ra8_unarch` (tar walker, gzip member, XZ stream) consumes
 * its untrusted input through the same seek+read function shape the streaming
 * EPUB / CBZ / CBR readers already use, so one `ra8_fs` file or `ra8_vmem`
 * page-cache backing drives every container format identically. This header
 * owns that typedef plus the one trivial backing everybody needs: a bounded
 * flat-memory view (::ra8_unarch_mem_t + ::ra8_unarch_mem_read) used to
 * re-open the decompressed bytes of a wrapped archive (`.tar.gz`, `.tar.xz`)
 * without copying them again.
 *
 * @note Thread-safety follows the backing: the flat-memory reader is a pure
 *       read and safe anywhere; file-backed readers are single-threaded.
 *
 * @see ra8_unarch_tar.h  The tar walker driven through this seam.
 * @see ra8_unarch_gzip.h The gzip member decoder driven through this seam.
 * @see ra8_unarch_xz.h   The XZ stream decoder driven through this seam.
 *
 * @since Version 0.1.0
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @typedef ra8_unarch_read_fn
 * @brief Seek+read backing over an archive's bytes.
 * @details Identical in shape to `ra8_comic_read_fn` / `ra8_rar_read_fn`: an
 *          absolute offset plus a byte count in, bytes-actually-read out. A
 *          return shorter than @p len signals end-of-file (or a backing
 *          error); the decoders treat any short read fail-closed.
 * @param[in]  ctx    Opaque backing context.
 * @param[in]  offset Absolute byte offset within the archive.
 * @param[out] buf    Destination buffer (@p len writable bytes).
 * @param[in]  len    Bytes requested.
 * @return Bytes actually read (0 at/after EOF or on error).
 * @note Thread-safety follows the backing implementation.
 * @since Version 0.1.0
 */
typedef size_t (*ra8_unarch_read_fn)(void* ctx, uint64_t offset, void* buf, size_t len);

/**
 * @struct ra8_unarch_mem_t
 * @brief Bounded flat-memory backing for ::ra8_unarch_read_fn.
 *
 * @details Describes `[base, base + len)` as a readable archive. Used by the
 *          wrapped-open paths: the unwrapped (decompressed) bytes live in a
 *          caller arena and this descriptor re-presents them through the
 *          common read seam. The descriptor must out-live every read through
 *          it, so callers embed it next to the arena it describes.
 *
 * @invariant `base` addresses at least `len` readable bytes while any
 *            reader holds this descriptor.
 *
 * @par Example:
 * @code
 * ra8_unarch_mem_t mem = {.base = arena, .len = unwrapped_len};
 * size_t got = ra8_unarch_mem_read(&mem, 0U, hdr, sizeof(hdr));
 * @endcode
 *
 * @see ra8_unarch_mem_read()
 * @since Version 0.1.0
 */
typedef struct {
  const uint8_t* base; /**< First readable byte of the flat view. */
  size_t         len;  /**< Readable length in bytes.             */
} ra8_unarch_mem_t;

/**
 * @brief ::ra8_unarch_read_fn over a flat ::ra8_unarch_mem_t view.
 *
 * @details Serves `[offset, offset + len)` clamped to the view's bounds by
 *          plain copy. Reads at or past the end return 0 (EOF); a NULL
 *          context or view base also returns 0, which every decoder treats
 *          as a failed read (fail-closed).
 *
 * @param[in]  ctx    The ::ra8_unarch_mem_t view (may be NULL: reads 0).
 * @param[in]  offset Absolute byte offset within the view.
 * @param[out] buf    Destination buffer (@p len writable bytes, non-NULL).
 * @param[in]  len    Bytes requested.
 *
 * @return Bytes actually copied.
 * @retval 0 NULL view / NULL destination, EOF, or a zero-length request.
 *
 * @pre @p buf holds @p len writable bytes when @p len > 0.
 * @pre The view's `base`/`len` describe readable storage.
 * @post At most `len` bytes of @p buf are written; nothing else changes.
 * @post The view itself is never modified (pure read).
 *
 * @note Thread-safe: pure read of an immutable view.
 * @see ra8_unarch_mem_t
 * @since Version 0.1.0
 */
[[nodiscard]] size_t ra8_unarch_mem_read(void* ctx, uint64_t offset, void* buf, size_t len);

#ifdef __cplusplus
}
#endif
