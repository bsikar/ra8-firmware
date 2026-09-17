/**
 * @file examples/ek_ra8d2/hil_needs_revalidation/ereader_shelf/inc/sh_classify.h
 * @brief Book-format classification by file extension (long-name aware, #633).
 *
 * @details
 * The shelf scans the FAT root and decides which files are books and in what
 * container format. Since #600 gave `ra8_fs` VFAT long-name write, the card
 * carries real extensions (`.rabook`, `.epub`) rather than the 8.3 truncations
 * (`.RBK`, `.EPB`) the tools used to emit, so the classifier matches BOTH: the
 * long forms and, so existing 8.3-named cards still resolve, the legacy short
 * forms. Matching is case-insensitive because FAT hands back 8.3 names
 * upper-cased while long names keep their authored case.
 *
 * This header is board-free (string logic only), so `sh_sd.c` and the host
 * classification test share one implementation instead of a copy.
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
 * @enum sh_book_fmt_t
 * @brief Book container format behind a shelf entry / the open book.
 * @details Both text formats render through the same screens via the sh_book.c
 *          backend: `.rabook` is the pre-parsed book blob, demand-paged
 *          through the chunked reader (baked or on SD); `.epub` is parsed
 *          on-device by epub (SD only). The three comic containers route
 *          through sh_comic.c as image-page readers.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_sh_fmt_rabook = 0U, /**< book RBKC container (demand-paged chunks).  */
  k_sh_fmt_epub   = 1U, /**< EPUB parsed on-device by epub.              */
  k_sh_fmt_cbz    = 2U, /**< Comic archive: ZIP of page images (`.cbz`). */
  k_sh_fmt_cbr    = 3U, /**< Comic archive: RAR of page images (`.cbr`). */
  k_sh_fmt_cbt    = 4U, /**< Comic archive: tar of page images (`.cbt`). */
} sh_book_fmt_t;

/**
 * @brief True if @p fmt is a comic-archive container (CBZ, CBR, or CBT).
 * @details Both route through sh_comic.c (::comic) rather than the
 *          text-book screens, so this predicate is the single dispatch test
 *          the shelf/open path branches on.
 * @param[in] fmt Container format from a shelf entry / the open book.
 * @return true for ::k_sh_fmt_cbz, ::k_sh_fmt_cbr, or ::k_sh_fmt_cbt.
 * @retval true  @p fmt is a comic archive (image-page reader).
 * @retval false @p fmt is a text book (rabook / epub).
 * @pre @p fmt is a valid ::sh_book_fmt_t.
 * @post No state is modified (pure predicate).
 * @note Thread-safe: pure function of its argument.
 * @since 0.1.0
 */
static inline bool sh_fmt_is_comic(sh_book_fmt_t fmt)
{
  return (fmt == k_sh_fmt_cbz) || (fmt == k_sh_fmt_cbr) || (fmt == k_sh_fmt_cbt);
}

/**
 * @brief Fold one ASCII letter to lower case (non-letters returned unchanged).
 * @param[in] c Byte to fold.
 * @return The lower-case letter, or @p c unchanged when it is not `A`..`Z`.
 * @retval char Case-folded byte.
 * @pre @p c is a single byte.
 * @post No state is modified (pure function).
 * @note Thread-safe: pure function of its argument.
 * @since 0.1.0
 */
static inline char sh_ascii_lower(char c)
{
  return ((c >= 'A') && (c <= 'Z')) ? (char)(c + ('a' - 'A')) : c;
}

/**
 * @brief Case-insensitive test that @p name ends in the extension @p ext.
 * @details Requires at least one character before @p ext, so a name that is
 *          only the extension (e.g. `.epub`) is not a book. Folds both sides to
 *          lower case, so `.EPB` matches `.epb` and `Book.EPUB` matches `.epub`.
 * @param[in] name NUL-terminated file name (non-NULL).
 * @param[in] ext  NUL-terminated extension including its dot, e.g. `.rabook`.
 * @return Whether @p name ends in @p ext with a non-empty stem.
 * @retval true  @p name is longer than @p ext and its tail case-matches @p ext.
 * @retval false @p name is not longer than @p ext or the tail differs.
 * @pre @p name and @p ext are non-NULL and NUL-terminated.
 * @pre @p ext is a file extension leading with `.`.
 * @post No state is modified (pure function).
 * @note Thread-safe: pure function over its arguments.
 * @since 0.1.0
 */
static inline bool sh_ext_match(const char* name, const char* ext)
{
  const size_t n = strlen(name);
  const size_t e = strlen(ext);
  if (n <= e) {
    return false;
  }
  for (size_t i = 0U; i < e; ++i) {
    if (sh_ascii_lower(name[n - e + i]) != sh_ascii_lower(ext[i])) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Classify a root file name into a book format; true if it is a book.
 * @details Recognises the long extensions the tools now write (`.rabook`,
 *          `.epub`) and, so existing 8.3-named cards keep resolving, their
 *          legacy 8.3 truncations (`.rbk`, `.epb`). Comics already had 3-char
 *          extensions, so `.cbz` / `.cbr` / `.cbt` never truncated. Matching is
 *          case-insensitive (@ref sh_ext_match).
 * @param[in]  name    NUL-terminated root file name (non-NULL).
 * @param[out] out_fmt Receives the container format when @p name is a book.
 * @return Whether @p name names a book this shelf can open.
 * @retval true  @p name matched a known book extension; `*out_fmt` is set.
 * @retval false @p name is not a book; `*out_fmt` is untouched.
 * @pre @p name and @p out_fmt are non-NULL.
 * @pre @p name is a single root directory entry name.
 * @post On true `*out_fmt` holds the classified ::sh_book_fmt_t.
 * @post On false no output is written.
 * @note Thread-safe: writes only @p out_fmt.
 * @since 0.1.0
 */
static inline bool sh_book_classify(const char* name, sh_book_fmt_t* out_fmt)
{
  if (sh_ext_match(name, ".rabook") || sh_ext_match(name, ".rbk")) {
    *out_fmt = k_sh_fmt_rabook;
    return true;
  }
  if (sh_ext_match(name, ".epub") || sh_ext_match(name, ".epb")) {
    *out_fmt = k_sh_fmt_epub;
    return true;
  }
  if (sh_ext_match(name, ".cbz")) {
    *out_fmt = k_sh_fmt_cbz;
    return true;
  }
  if (sh_ext_match(name, ".cbr")) {
    *out_fmt = k_sh_fmt_cbr;
    return true;
  }
  if (sh_ext_match(name, ".cbt")) {
    *out_fmt = k_sh_fmt_cbt;
    return true;
  }
  return false;
}
