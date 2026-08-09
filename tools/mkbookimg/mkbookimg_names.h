/**
 * @file mkbookimg_names.h
 * @brief Derive an on-card file name from a source book path (long-name capable).
 *
 * @details
 * mkbookimg packs each input book onto the FAT card under a HUMAN-READABLE
 * name: the source file's own basename, stored verbatim now that `ra8_fs`
 * writes VFAT long names (#600). Before that landed the tool emitted `BOOK01`,
 * `BOOK02`, ... under a forced 8.3 extension and threw the real name away
 * (#633 removed that workaround).
 *
 * This header is the pure, board-free name derivation the tool and its host
 * round-trip test both call, so the test exercises the SAME logic the packer
 * ships rather than a copy of it.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/**
 * @enum mkbookimg_name_limit_t
 * @brief Bound on a derived on-card file name.
 * @details The buffer must hold the widest VFAT long name `ra8_fs` will file
 *          (247 characters) plus a NUL, so 256 clears it with headroom while
 *          keeping the derivation on a fixed, statically-bounded buffer.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_mkbookimg_name_cap = 256U, /**< Destination-name buffer bytes incl. NUL (>= FAT 247 + 1). */
} mkbookimg_name_limit_t;

/**
 * @brief Return the basename of @p path: the run after its last '/'.
 *
 * @details Scans @p path once for the final path separator and returns a
 *          pointer just past it, so `content/library/Meditations.rabook`
 *          yields `Meditations.rabook` and a already-bare name is returned
 *          unchanged. Only '/' is treated as a separator (the tool takes POSIX
 *          host paths on its command line).
 *
 * @param[in] path NUL-terminated source path (non-NULL).
 *
 * @return Pointer into @p path at the first character after the last '/', or
 *         @p path itself when it contains no separator.
 * @retval non-NULL Always a valid pointer within @p path (never past its NUL).
 *
 * @pre @p path is non-NULL and NUL-terminated.
 * @pre @p path is a host filesystem path (separators are '/').
 * @post The returned pointer aliases @p path; no bytes are copied or modified.
 * @post The result points at a NUL when @p path ends in '/'.
 *
 * @note Thread-safe: pure function over its argument.
 * @since 0.1.0
 */
static inline const char* mkbookimg_basename(const char* path)
{
  size_t start = 0U;
  size_t i     = 0U;
  for (; path[i] != '\0'; ++i) {
    if (path[i] == '/') {
      start = i + 1U;
    }
  }
  return &path[start];
}

/**
 * @brief Derive the on-card destination name for a source book @p path.
 *
 * @details Copies the source's basename (@ref mkbookimg_basename) verbatim into
 *          @p out and NUL-terminates it, so the name the caller sees on the card
 *          is the name they packed. The copy is rejected -- rather than
 *          truncated -- when the basename is empty or does not fit @p cap, so a
 *          name is either filed in full or refused.
 *
 * @param[in]  path Source path on the host (non-NULL, NUL-terminated).
 * @param[out] out  Destination-name buffer; receives the NUL-terminated name.
 * @param[in]  cap  Capacity of @p out in bytes (> 0; use @ref k_mkbookimg_name_cap).
 *
 * @return Whether a usable destination name was written.
 * @retval true  A non-empty basename that fit @p cap was written and terminated.
 * @retval false @p path / @p out is NULL, @p cap is 0, the basename is empty, or
 *               it does not fit @p cap (nothing usable is left in @p out).
 *
 * @pre @p path is non-NULL and NUL-terminated.
 * @pre @p out points at @p cap writable bytes.
 * @post On true, @p out holds the basename of @p path including its terminator.
 * @post On false, @p out is not relied upon (no truncated name is filed).
 *
 * @note Thread-safe: writes only @p out.
 * @since 0.1.0
 */
static inline bool mkbookimg_dest_name(const char* path, char* out, size_t cap)
{
  if ((path == nullptr) || (out == nullptr) || (cap == 0U)) {
    return false;
  }
  const char*  base = mkbookimg_basename(path);
  const size_t n    = strlen(base);
  if ((n == 0U) || (n >= cap)) {
    return false;
  }
  memcpy(out, base, n);
  out[n] = '\0';
  return true;
}
