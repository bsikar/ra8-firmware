/**
 * @file inc/mkbookimg_internal.h
 * @brief Module-private seams shared by the two mkbookimg translation units.
 *
 * @details
 * The image builder is split across two translation units to keep each under
 * the maintainability line cap:
 *
 *  - `src/mkbookimg.c`       -- the host container: descriptor-level diagnostics,
 *    exact positioned host I/O, the descriptor-backed `ra8_fs` block device,
 *    the atomic sibling-temporary publication, and the `main` entry point.
 *  - `src/mkbookimg_books.c` -- the population pass: opening and identifying each
 *    immutable host input, streaming it into the mounted volume, and proving
 *    the stored copy byte-for-byte before the image may be published.
 *
 * This header carries the fixed image geometry (::mkbookimg_limit_t), the
 * descriptor-backed device state (::mkbookimg_disk_t), the open-flag fallbacks
 * both units need, and the `RA8_PRIV` declarations of every helper shared
 * across them. Nothing here is a reusable surface: the tool links `ra8_fs` and
 * publishes no library of its own.
 *
 * [Ring 7 / Tooling] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
#pragma once

#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_fs.h"

#ifndef O_CLOEXEC
/** @brief No-op close-on-exec fallback for hosts lacking the flag. */
#define O_CLOEXEC (0)
#endif

#ifndef O_DIRECTORY
/** @brief No-op directory-open fallback for hosts lacking the flag. */
#define O_DIRECTORY (0)
#endif

#ifndef O_NOFOLLOW
/** @brief No-op no-follow fallback for hosts lacking the flag. */
#define O_NOFOLLOW (0)
#endif

/** @brief Fixed image geometry and bounded host-I/O capacities. */
typedef enum : uint32_t {
  k_block_size           = 512U,        /**< Bytes per image sector.       */
  k_img_sectors          = 131072U,     /**< Sectors in the 64 MiB image.  */
  k_max_books            = 32U,         /**< Maximum input-book count.     */
  k_stream_chunk_bytes   = 16U * 1024U, /**< Bounded copy chunk bytes.     */
  k_host_path_cap        = 4096U,       /**< Hosted path capacity.         */
  k_host_name_cap        = 256U,        /**< Hosted leaf capacity.         */
  k_temp_create_attempts = 128U,        /**< Exclusive-create retry bound. */
  k_decimal_u64_digits   = 20U,         /**< Maximum `uint64_t` digits.    */
  k_decimal_base         = 10U,         /**< Decimal conversion radix.     */
  k_output_create_mode   = 0666U,       /**< Hosted output creation mode.  */
} mkbookimg_limit_t;

/** @brief Host descriptor-backed block device bound into `ra8_fs`. */
typedef struct {
  uint64_t block_count; /**< Addressable sectors.               */
  uint32_t block_size;  /**< Bytes in one sector.               */
  int      fd;          /**< Open sibling-temporary descriptor. */
  bool     io_failed;   /**< Sticky positioned-I/O failure.     */
} mkbookimg_disk_t;

/**
 * @brief Write a complete diagnostic fragment to the standard-error descriptor.
 * @details Retries interrupted and short writes without involving stdio streams.
 * @param[in] text NUL-terminated fragment.
 * @pre @p text is non-NULL and NUL-terminated.
 * @pre Standard error may be written or may reject the diagnostic.
 * @post The complete fragment was attempted.
 * @post No application state changed.
 * @note Fragments may interleave with another process's diagnostics.
 * @since 0.1.0
 */
RA8_PRIV void priv_mkbookimg_diag(const char* text);

/**
 * @brief Emit an unsigned decimal value without a formatting runtime.
 * @details Converts into fixed local buffers and forwards one bounded string.
 * @param[in] value Value to emit.
 * @pre Standard error may be written or may reject the diagnostic.
 * @pre ::k_decimal_u64_digits holds every uint64 decimal spelling.
 * @post The decimal spelling was attempted on standard error.
 * @post No application state changed.
 * @note Fragments may interleave with another process's diagnostics.
 * @since 0.1.0
 */
RA8_PRIV void priv_mkbookimg_diag_u64(uint64_t value);

/**
 * @brief Read exactly one bounded positioned byte range.
 * @details Retries interrupted calls and rejects EOF before the requested end.
 * @param[in]  fd     Open regular-file descriptor.
 * @param[in]  offset Absolute starting byte offset.
 * @param[out] bytes  Destination spanning @p length bytes.
 * @param[in]  length Exact byte count.
 * @return true only when every byte was read.
 * @retval true  The destination contains the complete requested range.
 * @retval false EOF, overflow, or an unrecoverable read error occurred.
 * @pre @p bytes spans @p length writable bytes.
 * @pre The descriptor remains open for this call.
 * @post Descriptor position is unchanged.
 * @post On false @p bytes may hold a strict prefix.
 * @note Thread-safe for independent buffers while the descriptor remains open.
 * @since 0.1.0
 */
RA8_PRIV bool priv_mkbookimg_pread_exact(int fd, uint64_t offset, uint8_t* bytes, size_t length);

/**
 * @brief Format, populate, and unmount the destination image.
 * @details Formats a fresh FAT32 volume, writes every input book, and
 *          unmounts before reporting the combined outcome.
 * @param[in] backend Block-device backend bound to @p disk.
 * @param[in] argv Output path followed by book paths.
 * @param[in] book_count Number of book paths in @p argv.
 * @param[in,out] disk Block-device state; inspected for a deferred I/O fault.
 * @return Whether format, every book write, and unmount all succeeded.
 * @retval true The destination image is complete and consistent.
 * @retval false Formatting, a book write, unmount, or block I/O failed.
 * @pre @p backend is bound to @p disk and @p disk names an open image file.
 * @pre @p book_count is in 1..::k_max_books, already validated by the caller.
 * @post On true the image volume is unmounted and internally consistent.
 * @post No mount handle remains claimed on any return path.
 * @note Not thread-safe through global `ra8_fs` slots.
 * @since 0.1.0
 */
RA8_PRIV bool priv_mkbookimg_build_image(const ra8_fs_backend_t* backend,
                                         char**                  argv,
                                         int                     book_count,
                                         const mkbookimg_disk_t* disk);
