/**
 * @file ra_comic_cbr.c
 * @brief CBR backend: walk a RAR archive (ra_rar.h) and index its page images.
 *
 * @par Tag
 * [Ring 4 / Domain] {World: NS}
 *
 * @details
 * The RAR half of the comic facade. ::ra_comic_cbr_open walks the RAR block chain
 * one header at a time through the clean-room ::ra_rar_next, appending each image
 * file member to the shared page index; a member packed with a RAR compressor is
 * indexed with `extractable == 0` so the page list stays complete while
 * ::ra_comic_cbr_extract reports it unsupported. STORE members -- the common case
 * for comics whose pages are already-compressed JPEG/PNG -- extract by streaming
 * the member's data area. The walk stops at the first unparseable block, keeping
 * the pages found so far (tolerant of trailing junk).
 *
 * Zero allocation (NASA Rule 3): the walker reads one header into a stack scratch
 * and streams one member through the caller's buffer.
 *
 * @since Version 0.1.0
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include <stddef.h>
#include <stdint.h>

#include "ra_check.h"
#include "ra_comic.h"
#include "ra_comic_internal.h"
#include "ra_rar.h"

/** @brief Log tag for CBR-backend diagnostics. */
static const char* const s_tag_cbr = "ra_comic_cbr";

/**
 * @enum cbr_limits_t
 * @brief Name scratch size and the hard block-walk bound.
 * @details ::k_cbr_max_blocks statically bounds the walk (NASA Rule 2); a comic's
 *          block count is far below it, and ::ra_rar_next guarantees a strictly
 *          advancing offset so the archive size is the real terminator.
 * @since Version 0.1.0
 */
typedef enum : uint32_t {
  k_cbr_name_max   = 256U,    /**< Longest member name indexed (bytes, incl. path). */
  k_cbr_max_blocks = 100000U, /**< Static upper bound on blocks walked.             */
} cbr_limits_t;

/**
 * @brief Index one RAR block if it is a decodable page-image file member.
 * @details Skips non-file blocks, directories and non-image / hidden names;
 *          otherwise appends the member (marking `extractable` from the STORE
 *          method) to the shared index.
 * @param[in,out] c    Comic accumulating its index.
 * @param[in]     ent  Decoded block from ::ra_rar_next.
 * @param[in]     name Member name bytes.
 * @param[in]     nlen Member name length.
 * @return ra_err_t status (k_ra_ok also for a skipped, non-page block).
 * @retval k_ra_ok               Member indexed or intentionally skipped.
 * @retval k_ra_err_invalid_size Page index / name arena is full.
 * @pre @p ent came from ::ra_rar_next; @p name holds @p nlen bytes.
 * @pre @p c has its buffers bound.
 * @post On k_ra_ok either one page was added or nothing changed (a skip).
 * @post On error the index is unchanged.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
static ra_err_t
s_add_member(ra_comic_t* c, const ra_rar_entry_t* ent, const char* name, uint16_t nlen)
{
  if (ent->is_file == 0U) {
    return k_ra_ok;
  }
  if (ent->is_dir != 0U) {
    return k_ra_ok;
  }
  if (!ra_comic_is_page_name(name, nlen)) {
    return k_ra_ok;
  }
  const uint8_t extractable = (ent->method == (uint8_t)k_ra_rar_method_store) ? 1U : 0U;
  return ra_comic_page_add(c,
                           name,
                           nlen,
                           ent->unp_size,
                           extractable,
                           0U,
                           ent->data_off,
                           ent->pack_size);
}

ra_err_t ra_comic_cbr_open(ra_comic_t* c)
{
  RA_CHECK_NULL_PTR(c, s_tag_cbr, "cbr open: null c");
  if (c->rar.version == k_ra_rar_ver_none) {
    return k_ra_err_invalid_state;
  }
  uint64_t off = c->rar.first_off;
  for (uint32_t n = 0U; n < (uint32_t)k_cbr_max_blocks; ++n) { /* bound: static block cap */
    if (off >= c->rar.size) {
      break;
    }
    char           nb[k_cbr_name_max] = {};
    ra_rar_entry_t ent                = {};
    if (ra_rar_next(&c->rar, off, nb, (uint16_t)sizeof(nb), &ent) != k_ra_ok) {
      break; /* tolerate trailing junk: keep the pages parsed so far */
    }
    const ra_err_t ae = s_add_member(c, &ent, nb, ent.name_len);
    if (ae != k_ra_ok) {
      return ae; /* index / arena full -- a real capacity error, propagate */
    }
    off = ent.next_off;
  }
  return k_ra_ok;
}

ra_err_t
ra_comic_cbr_extract(ra_comic_t* c, const ra_comic_page_t* p, uint8_t* buf, size_t cap, size_t* got)
{
  RA_CHECK_NULL_PTR(c, s_tag_cbr, "cbr extract: null c");
  RA_CHECK_NULL_PTR(p, s_tag_cbr, "cbr extract: null p");
  RA_CHECK_NULL_PTR(buf, s_tag_cbr, "cbr extract: null buf");
  RA_CHECK_NULL_PTR(got, s_tag_cbr, "cbr extract: null got");
  *got = 0U;
  if (c->rar.version == k_ra_rar_ver_none) {
    return k_ra_err_invalid_state;
  }
  if (p->extractable == 0U) {
    return k_ra_err_not_supported;
  }
  ra_rar_entry_t ent = {};
  ent.is_file        = 1U;
  ent.is_dir         = 0U;
  ent.method         = (uint8_t)k_ra_rar_method_store;
  ent.data_off       = p->data_off;
  ent.pack_size      = p->pack_size;
  ent.unp_size       = p->raw_size;
  return ra_rar_extract_stored(&c->rar, &ent, buf, cap, got);
}
