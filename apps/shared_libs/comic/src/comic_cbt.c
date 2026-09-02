/**
 * @file comic_cbt.c
 * @brief CBT backend: walk a tar archive (unarch_tar.h) and index its pages.
 *
 * @par Tag
 * [Ring 4 / Domain] {World: NS}
 *
 * @details
 * The tar half of the comic facade. ::priv_comic_cbt_open walks the tar member
 * chain one header at a time through the clean-room streaming walker
 * (`unarch_tar_next` -- ustar / pax / GNU dialects, every block charged
 * against the unified decompression-limits policy), appending each image file
 * member to the shared page index. tar stores members verbatim, so
 * ::priv_comic_cbt_extract is a bounds-revalidated literal copy -- the same
 * demand-paged model as the CBZ / CBR backends, with zero allocation.
 *
 * Unlike the tolerant CBR walk (RAR archives often carry trailing junk), a
 * malformed tar header rejects the archive: tar's block grammar leaves no
 * legitimate reason for an undecodable header mid-chain.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since Version 0.1.0
 *
 */
#include <stddef.h>
#include <stdint.h>

#include "comic.h"
#include "comic_internal.h"
#include "ra8_attributes.h"
#include "ra8_check.h"
#include "unarch_tar.h"

/** @brief Log tag for CBT-backend diagnostics. */
static const char* const s_tag_cbt = "comic_cbt";

/**
 * @enum cbt_limits_t
 * @brief Name scratch size for the tar member walk.
 * @since Version 0.1.0
 */
typedef enum : uint32_t {
  k_cbt_name_max    = 256U,    /**< Longest member name indexed (bytes, incl. path). */
  k_cbt_max_members = 100000U, /**< Static upper bound on members walked.            */
} cbt_limits_t;

/**
 * @brief Index one tar member if it is a decodable page-image file.
 * @details Skips directories, non-file members, and non-image / hidden
 *          names; otherwise appends the member. tar members are stored
 *          verbatim, so `pack_size == raw_size` and every page decodes.
 * @param[in,out] c    Comic accumulating its index.
 * @param[in]     ent  Decoded member from ::unarch_tar_next.
 * @param[in]     name Member name bytes.
 * @param[in]     nlen Member name length.
 * @return ra8_err_t status (k_ra8_ok also for a skipped, non-page member).
 * @retval k_ra8_ok               Member indexed or intentionally skipped.
 * @retval k_ra8_err_invalid_size Page index / name arena is full.
 * @pre @p ent came from ::unarch_tar_next; @p name holds @p nlen bytes.
 * @pre @p c has its buffers bound.
 * @post On k_ra8_ok either one page was added or nothing changed (a skip).
 * @post On error the index is unchanged.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_add_member(comic_t* c, const unarch_tar_entry_t* ent, const char* name, uint16_t nlen)
{
  if (ent->is_file == 0U) {
    return k_ra8_ok;
  }
  if (!priv_comic_is_page_name(name, nlen)) {
    return k_ra8_ok;
  }
  return priv_comic_page_add(c,
                             name,
                             nlen,
                             ent->size,
                             1U,
                             (uint8_t)k_ra8_rar_method_store, /* stored verbatim */
                             0U,
                             ent->data_off,
                             ent->size);
}

RA8_PRIV ra8_err_t priv_comic_cbt_open(comic_t* c)
{
  RA8_CHECK_NULL_PTR(c, s_tag_cbt, "cbt open: null c");
  if (!c->tar.live) {
    return k_ra8_err_invalid_state;
  }
  uint64_t off = 0U;
  for (uint32_t n = 0U; n < (uint32_t)k_cbt_max_members; ++n) { /* bound: static member cap */
    char               nb[k_cbt_name_max] = {};
    unarch_tar_entry_t ent                = {};
    const ra8_err_t    nerr = unarch_tar_next(&c->tar, off, nb, (uint16_t)sizeof(nb), &ent);
    if (nerr == k_ra8_err_not_found) {
      return k_ra8_ok; /* clean end of archive */
    }
    if (nerr != k_ra8_ok) {
      return nerr; /* malformed / lying / over-policy member: reject */
    }
    const ra8_err_t ae = internal_add_member(c, &ent, nb, ent.name_len);
    if (ae != k_ra8_ok) {
      return ae; /* index / arena full -- a real capacity error, propagate */
    }
    off = ent.next_off;
  }
  return k_ra8_err_validation_failed; /* GCOVR_EXCL_LINE -- the walker's policy
                                       * entry cap (4096) fires inside tar_next
                                       * far below the static member cap */
}

RA8_PRIV ra8_err_t
priv_comic_cbt_extract(comic_t* c, const comic_page_t* p, uint8_t* buf, size_t cap, size_t* got)
{
  RA8_CHECK_NULL_PTR(c, s_tag_cbt, "cbt extract: null c");
  RA8_CHECK_NULL_PTR(p, s_tag_cbt, "cbt extract: null p");
  RA8_CHECK_NULL_PTR(buf, s_tag_cbt, "cbt extract: null buf");
  RA8_CHECK_NULL_PTR(got, s_tag_cbt, "cbt extract: null got");
  *got = 0U;
  if (!c->tar.live) {
    return k_ra8_err_invalid_state;
  }
  unarch_tar_entry_t ent = {};
  ent.is_file            = 1U;
  ent.data_off           = p->data_off;
  ent.size               = p->raw_size;
  return unarch_tar_read(&c->tar, &ent, buf, cap, got);
}
