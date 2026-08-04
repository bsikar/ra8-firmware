/**
 * @file ra8_unarch_tar.h
 * @brief Clean-room, read-only streaming tar walker (POSIX ustar + pax + GNU).
 * @ingroup grp_ereader
 *
 * @par Tag
 * [Ring 4 / Domain] {World: NS}
 *
 * @details
 * A first-party, hand-written tar *reader* for the comic-book-archive
 * (`.cbt`) and wrapped-tar (`.tar.gz` / `.tar.xz`) use cases. It walks the
 * 512-byte block chain of a POSIX ustar archive one header at a time over
 * the shared seek+read seam (`ra8_unarch_io.h`) and exposes each member
 * (name, size, data offset) without ever materialising the archive.
 * Supported header dialects, all parsed fail-closed:
 *
 * - **ustar** (POSIX.1-1988): the base 512-byte header with the
 *   `prefix`-field long-name split.
 * - **pax** (POSIX.1-2001): `x` extended headers; the `path` and `size`
 *   record overrides are applied to the following member, every other
 *   record is skipped. `g` global headers are skipped whole.
 * - **GNU**: `L` longname data blocks; base-256 (binary) size fields.
 *
 * Everything else -- link/device/fifo members, unknown typeflags -- is
 * enumerated as a non-file entry and skipped by size, so the walk never
 * stalls on exotic content.
 *
 * @par Clean-room licensing
 * Written from the POSIX.1-2017 `pax` Interchange Format specification and
 * the publicly documented GNU tar extensions. It vendors no code from GNU
 * tar, libarchive, or busybox, and is therefore first-party MIT (root
 * `LICENSE.txt`), not SOUP, and carries no SBOM entry.
 *
 * @par Bounded, fail-closed (the archive-hardening contract)
 * The walker charges every enumerated member and every examined header
 * block against the unified decompression-limits policy
 * (`ra8_decomp_limits.h`), so the many-tiny-entries bomb and the
 * meta-block-flood bomb both terminate within policy. A member whose
 * declared size breaks the policy's output cap, overruns the archive, or
 * whose header fails the checksum is rejected with a specific error.
 * Resident state is O(one header block); pax data is parsed through a
 * small fixed scratch and rejected when larger.
 *
 * @note Not thread-safe; the single-threaded reader loop serialises access.
 *
 * @see ra8_comic.h          The comic facade whose CBT backend drives this.
 * @see ra8_decomp_limits.h  The policy every walk is charged against.
 * @see ra8_unarch_io.h      The seek+read seam the walker consumes.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since Version 0.1.0
 *
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra8_decomp_limits.h"
#include "ra8_err.h"
#include "ra8_unarch_io.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @enum ra8_unarch_tar_dims_t
 * @brief Fixed sizes of the tar on-disk grammar and the walker's bounds.
 * @details The block size is fixed by every tar dialect; the meta and pax
 *          bounds are this walker's fail-closed limits on the pax / GNU
 *          prelude a single member may carry.
 * @since Version 0.1.0
 */
typedef enum : uint16_t {
  k_ra8_unarch_tar_block    = 512U,  /**< On-disk block / header size.          */
  k_ra8_unarch_tar_meta_max = 4U,    /**< Max meta (x/g/L/K) blocks per member. */
  k_ra8_unarch_tar_pax_max  = 2048U, /**< Max pax / longname data bytes.        */
} ra8_unarch_tar_dims_t;

/**
 * @struct ra8_unarch_tar_t
 * @brief One open tar archive: the backing plus the walk's running budget.
 *
 * @details Populated by ::ra8_unarch_tar_open; treat every field as
 *          read-only outside the API. The embedded budget accumulates
 *          across ::ra8_unarch_tar_next calls, so the policy's entry cap
 *          and iteration budget bound the whole enumeration, not one call.
 *
 * @invariant `read != NULL` and `live` between a successful open and close.
 * @invariant `budget.limits` is a validated policy.
 *
 * @see ra8_unarch_tar_open()
 * @since Version 0.1.0
 */
typedef struct {
  ra8_unarch_read_fn  read;   /**< Byte reader over the archive.          */
  void*               ctx;    /**< Context for @ref read.                 */
  uint64_t            size;   /**< Archive length in bytes.               */
  ra8_decomp_budget_t budget; /**< Entry / iteration budget for the walk. */
  bool                live;   /**< Open succeeded and not superseded.     */
} ra8_unarch_tar_t;

/**
 * @struct ra8_unarch_tar_entry_t
 * @brief One decoded tar member: a file, a directory, or a skipped block.
 *
 * @details Filled by ::ra8_unarch_tar_next. @p next_off always advances
 *          strictly past the member (header, meta prelude, and data
 *          blocks), so a caller can walk the archive with a bounded loop.
 *          For a file member (@p is_file == 1) the data area is
 *          `[data_off, data_off + size)` -- tar stores members verbatim,
 *          so @p size is both the packed and the unpacked length.
 *
 * @invariant `next_off > <member offset>` for every well-formed member.
 * @invariant `data_off + size <= <archive size>` (validated by the walker).
 *
 * @see ra8_unarch_tar_next()
 * @since Version 0.1.0
 */
typedef struct {
  uint64_t data_off; /**< Absolute offset of the member's data area.          */
  uint64_t size;     /**< Member data length in bytes (stored verbatim).      */
  uint64_t next_off; /**< Absolute offset of the next member's first block.   */
  uint16_t name_len; /**< Name bytes copied into the caller buffer (clamped). */
  uint8_t  is_file;  /**< 1 if this member is a regular file.                 */
  uint8_t  is_dir;   /**< 1 if this member is a directory.                    */
} ra8_unarch_tar_entry_t;

/**
 * @brief Whether a leading header block looks like a tar archive.
 *
 * @details Pure probe over the first ::k_ra8_unarch_tar_block bytes: the
 *          ustar/GNU magic at its fixed offset plus a valid header
 *          checksum. Used by container detection to route a `.cbt` without
 *          constructing a walker. tar has no leading magic bytes, so both
 *          tests are required to keep false positives out of the comic
 *          open path.
 *
 * @param[in] block Leading archive bytes (may be NULL).
 * @param[in] len   Readable length of @p block in bytes.
 *
 * @return Whether the bytes begin a tar archive.
 * @retval true  Magic and checksum both hold.
 * @retval false @p block is NULL, short, or not a tar header.
 *
 * @pre @p block holds @p len readable bytes when non-NULL.
 * @pre @p len is the true readable length (untrusted values are safe).
 * @post No state is modified (pure read).
 * @post The result depends only on the first block.
 *
 * @note Thread-safe: pure read.
 * @see ra8_unarch_tar_open()
 * @since Version 0.1.0
 */
[[nodiscard]] bool ra8_unarch_tar_probe(const uint8_t* block, size_t len);

/**
 * @brief Bind a walker to an archive and validate its first header block.
 *
 * @details Reads block 0 through @p read, requires the ustar/GNU magic and
 *          a valid checksum (rejecting non-tar bytes cheaply), and binds
 *          the walk's decompression budget to @p limits (or the default
 *          policy). Performs no other I/O; the walk proceeds through
 *          ::ra8_unarch_tar_next.
 *
 * @param[out] t      Walker to populate (caller-owned).
 * @param[in]  read   Byte reader over the archive (non-NULL).
 * @param[in]  ctx    Context passed to @p read.
 * @param[in]  size   Archive length in bytes (>= one block).
 * @param[in]  limits Policy to enforce, or NULL for the default.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Recognised tar archive; @p t bound.
 * @retval k_ra8_err_null_ptr      @p t or @p read was NULL.
 * @retval k_ra8_err_invalid_size  @p size is smaller than one block.
 * @retval k_ra8_err_invalid_arg   @p limits has a zero field.
 * @retval k_ra8_err_not_supported Block 0 is not a tar header.
 *
 * @pre @p read serves offsets `[0, size)` of the archive.
 * @pre @p t is a writable ::ra8_unarch_tar_t.
 * @post On k_ra8_ok, `t->live` is true and the walk starts at offset 0.
 * @post On any error @p t is left dead (`live == false`).
 *
 * @note Not thread-safe.
 * @see ra8_unarch_tar_next()
 * @since Version 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_unarch_tar_open(ra8_unarch_tar_t*          t,
                                            ra8_unarch_read_fn         read,
                                            void*                      ctx,
                                            uint64_t                   size,
                                            const ra8_decomp_limits_t* limits);

/**
 * @brief Decode the member at @p off and advance to the next member.
 *
 * @details Consumes the member's pax / GNU meta prelude (bounded by
 *          ::k_ra8_unarch_tar_meta_max blocks and
 *          ::k_ra8_unarch_tar_pax_max data bytes), decodes the real
 *          header, applies any `path` / `size` overrides, and fills
 *          @p out -- including `out->next_off`, the offset of the
 *          following member. The member name (pax path, GNU longname, or
 *          prefix-joined ustar name) is copied into @p name_buf, clamped
 *          to @p name_cap. Every examined block charges the walk's
 *          iteration budget and every returned member charges its entry
 *          budget; a member whose declared size breaks the policy's
 *          output cap or overruns the archive is rejected.
 *
 * @param[in,out] t        Walker bound by ::ra8_unarch_tar_open (non-NULL).
 * @param[in]     off      Absolute offset of the member's first block.
 * @param[out]    name_buf Buffer for the member name (non-NULL if
 *                         @p name_cap > 0).
 * @param[in]     name_cap Capacity of @p name_buf in bytes.
 * @param[out]    out      Decoded member descriptor (non-NULL).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                    Member decoded; @p out filled.
 * @retval k_ra8_err_null_ptr          @p t or @p out was NULL.
 * @retval k_ra8_err_invalid_state     @p t was never bound.
 * @retval k_ra8_err_not_found         End of archive (zero block or EOF).
 * @retval k_ra8_err_validation_failed A truncated / malformed / lying
 *                                     header, checksum failure, or an
 *                                     over-limit meta prelude.
 * @retval k_ra8_err_decomp_entries    The policy entry cap was crossed.
 * @retval k_ra8_err_decomp_iterations The block-examination budget ran out.
 * @retval k_ra8_err_decomp_output_cap A member declares more bytes than
 *                                     the policy's per-unit output cap.
 *
 * @pre @p t was populated by ::ra8_unarch_tar_open.
 * @pre @p off is 0 or a `next_off` from a previous call.
 * @post On k_ra8_ok, `out->next_off > off` (the walk strictly advances).
 * @post On any error @p out is left zeroed.
 *
 * @note Not thread-safe (charges the walker's embedded budget).
 * @see ra8_unarch_tar_read()
 * @since Version 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_unarch_tar_next(ra8_unarch_tar_t*       t,
                                            uint64_t                off,
                                            char*                   name_buf,
                                            uint16_t                name_cap,
                                            ra8_unarch_tar_entry_t* out);

/**
 * @brief Copy a file member's data area into the caller buffer.
 *
 * @details Streams `ent->size` verbatim bytes from
 *          `[ent->data_off, ent->data_off + ent->size)` through the
 *          archive reader into @p buf. tar stores members uncompressed,
 *          so this is the whole extraction step; the bounds re-checked
 *          here make a hand-forged entry harmless.
 *
 * @param[in]  t   Walker bound by ::ra8_unarch_tar_open (non-NULL).
 * @param[in]  ent A file member from ::ra8_unarch_tar_next (non-NULL,
 *                 `is_file == 1`).
 * @param[out] buf Destination for the member bytes (non-NULL).
 * @param[in]  cap Capacity of @p buf in bytes; must be >= `ent->size`.
 * @param[out] got Receives the number of bytes written (non-NULL).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                    Member copied; `*got == ent->size`.
 * @retval k_ra8_err_null_ptr          A required pointer was NULL.
 * @retval k_ra8_err_invalid_state     @p t was never bound.
 * @retval k_ra8_err_not_supported     @p ent is not a regular file member.
 * @retval k_ra8_err_no_mem            @p cap is smaller than `ent->size`.
 * @retval k_ra8_err_invalid_size      The member overruns the archive, or
 *                                     the reader returned a short read.
 *
 * @pre @p t was populated by ::ra8_unarch_tar_open.
 * @pre @p ent came from ::ra8_unarch_tar_next on the same @p t.
 * @post On k_ra8_ok, `buf[0..*got)` holds the member's literal bytes.
 * @post On any error @p buf contents are unspecified and `*got == 0`.
 *
 * @note Not thread-safe.
 * @see ra8_unarch_tar_next()
 * @since Version 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_unarch_tar_read(const ra8_unarch_tar_t*       t,
                                            const ra8_unarch_tar_entry_t* ent,
                                            uint8_t*                      buf,
                                            size_t                        cap,
                                            size_t*                       got);

#ifdef __cplusplus
}
#endif
