/**
 * @file comic_cbr.c
 * @brief CBR backend: walk a RAR archive (ra8_rar.h) and index its page images.
 *
 * @par Tag
 * [Ring 4 / Domain] {World: NS}
 *
 * @details
 * The RAR half of the comic facade. ::priv_comic_cbr_open walks the RAR block chain
 * one header at a time through the clean-room ::ra8_rar_next, appending each image
 * file member to the shared page index. STORE members -- the common case for comics
 * whose pages are already-compressed JPEG/PNG -- extract by streaming the member's
 * data area; a RAR5-compressed member is inflated by the clean-room decompressor
 * (::ra8_rar5_decompress) through ::ra8_rar_extract. Only a RAR4-compressed member
 * (legacy codec, out of scope) is indexed `extractable == 0` and reported
 * unsupported. The walk stops at the first unparseable block, keeping the pages
 * found so far (tolerant of trailing junk).
 *
 * Zero allocation (NASA Rule 3): the walker reads one header into a stack scratch,
 * streams STORE members through the caller's buffer, and inflates compressed members
 * through the comic's caller-owned RAR5 scratch pool.
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
#include "ra8_decomp_limits.h"
#include "ra8_rar.h"

/** @brief Log tag for CBR-backend diagnostics. */
static const char* const s_tag_cbr = "comic_cbr";

/**
 * @enum cbr_limits_t
 * @brief Name scratch size and the hard block-walk bound.
 * @details ::k_cbr_max_blocks statically bounds the walk (NASA Rule 2); a comic's
 *          block count is far below it, and ::ra8_rar_next guarantees a strictly
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
 *          otherwise appends the member. A STORE member and, on a RAR5 archive, a
 *          compressed member are both `extractable` (the RAR5 decompressor decodes
 *          the latter); only a RAR4-compressed member stays non-extractable.
 * @param[in,out] c    Comic accumulating its index.
 * @param[in]     ent  Decoded block from ::ra8_rar_next.
 * @param[in]     name Member name bytes.
 * @param[in]     nlen Member name length.
 * @return ra8_err_t status (k_ra8_ok also for a skipped, non-page block).
 * @retval k_ra8_ok               Member indexed or intentionally skipped.
 * @retval k_ra8_err_invalid_size Page index / name arena is full.
 * @pre @p ent came from ::ra8_rar_next; @p name holds @p nlen bytes.
 * @pre @p c has its buffers bound.
 * @post On k_ra8_ok either one page was added or nothing changed (a skip).
 * @post On error the index is unchanged.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
internal_add_member(comic_t* c, const ra8_rar_entry_t* ent, const char* name, uint16_t nlen)
{
  if (ent->is_file == 0U) {
    return k_ra8_ok;
  }
  if (ent->is_dir != 0U) {
    return k_ra8_ok;
  }
  if (!priv_comic_is_page_name(name, nlen)) {
    return k_ra8_ok;
  }
  /* Decompression-limits guard: a page whose block header declares an
   * over-cap or bomb-ratio size rejects the whole archive before any
   * decode (the same policy every decoder enforces). */
  const ra8_decomp_limits_t lim  = ra8_decomp_limits_default();
  const ra8_err_t           derr = ra8_decomp_check_declared(&lim, ent->pack_size, ent->unp_size);
  if (derr != k_ra8_ok) {
    return derr;
  }
  const bool is_store = (ent->method == (uint8_t)k_ra8_rar_method_store);
  const bool is_rar5  = (c->rar.version == k_ra8_rar_ver_5);
  /* STORE always decodes; a compressed member decodes only on RAR5 (the RAR4
   * legacy codec is out of scope), so a RAR4-compressed page stays inert. */
  const uint8_t extractable = (is_store || is_rar5) ? 1U : 0U;
  return priv_comic_page_add(c,
                             name,
                             nlen,
                             ent->unp_size,
                             extractable,
                             ent->method,
                             0U,
                             ent->data_off,
                             ent->pack_size);
}

RA8_PRIV ra8_err_t priv_comic_cbr_open(comic_t* c)
{
  RA8_CHECK_NULL_PTR(c, s_tag_cbr, "cbr open: null c");
  if (c->rar.version == k_ra8_rar_ver_none) {
    return k_ra8_err_invalid_state;
  }
  /* Decompression-limits budget for the whole enumeration: every walked
   * block charges the entry cap, so the many-tiny-blocks bomb dies within
   * policy long before the static loop cap. Direct field bind (not
   * ra8_decomp_budget_init) because the default policy cannot fail
   * validation and the counters start zeroed. */
  ra8_decomp_budget_t budget = {};
  budget.limits              = ra8_decomp_limits_default();
  uint64_t off               = c->rar.first_off;
  for (uint32_t n = 0U; n < (uint32_t)k_cbr_max_blocks; ++n) {
    if (off >= c->rar.size) {
      break;
    }
    const ra8_err_t ce = ra8_decomp_budget_charge_entry(&budget);
    if (ce != k_ra8_ok) {
      return ce; /* block-flood bomb: reject the archive */
    }
    char            nb[k_cbr_name_max] = {};
    ra8_rar_entry_t ent                = {};
    if (ra8_rar_next(&c->rar, off, nb, (uint16_t)sizeof(nb), &ent) != k_ra8_ok) {
      /* Tolerate trailing junk and let the loop's sole exit handle it. */
      off = c->rar.size;
      continue;
    }
    const ra8_err_t ae = internal_add_member(c, &ent, nb, ent.name_len);
    if (ae != k_ra8_ok) {
      return ae; /* capacity / policy breach -- a real error, propagate */
    }
    off = ent.next_off;
  }
  return k_ra8_ok;
}

RA8_PRIV ra8_err_t
priv_comic_cbr_extract(comic_t* c, const comic_page_t* p, uint8_t* buf, size_t cap, size_t* got)
{
  RA8_CHECK_NULL_PTR(c, s_tag_cbr, "cbr extract: null c");
  RA8_CHECK_NULL_PTR(p, s_tag_cbr, "cbr extract: null p");
  RA8_CHECK_NULL_PTR(buf, s_tag_cbr, "cbr extract: null buf");
  RA8_CHECK_NULL_PTR(got, s_tag_cbr, "cbr extract: null got");
  *got = 0U;
  if (c->rar.version == k_ra8_rar_ver_none) {
    return k_ra8_err_invalid_state;
  }
  if (p->extractable == 0U) {
    return k_ra8_err_not_supported;
  }
  ra8_rar_entry_t ent = {};
  ent.is_file         = 1U;
  ent.is_dir          = 0U;
  ent.method          = p->rar_method;
  ent.data_off        = p->data_off;
  ent.pack_size       = p->pack_size;
  ent.unp_size        = p->raw_size;
  return ra8_rar_extract(&c->rar, &ent, buf, cap, &c->rar5_state, got);
}
