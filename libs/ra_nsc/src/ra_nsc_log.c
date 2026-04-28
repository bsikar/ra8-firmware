/**
 * @file ra_nsc_log.c
 * @brief NSC veneer: secure-side logging
 *
 * @par Tag
 * [Ring 4 / NSC] {World: NSC}
 *
 * @details
 * scaffold. Forwards the (tag, message) pair to
 * ``ra_log_info`` after copying both strings into a small
 * secure-side scratch area to make sure the secure code never
 * dereferences NS pointers directly.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_nsc.h"
#include "ra_nsc_veneer.h"

static const char* s_tag = "NSCLOG";

/**
 * @var s_tag_scratch
 * @brief Secure scratch for the (tag, message) copy.
 *
 * @details
 * Living in the secure data section, this buffer is updated
 * before each ra_log_info call so the secure code never has to
 * read from a Non-Secure pointer.
 */
static char s_tag_scratch[k_ra_nsc_log_msg_max_len];
static char s_msg_scratch[k_ra_nsc_log_msg_max_len];

/**
 * @brief Length-bounded string copy with NUL terminator.
 */
static void internal_safe_strcpy(char* dst, const char* src, uint32_t cap)
{
  uint32_t i = 0U;
  for (i = 0U; i < (cap - 1U); ++i) {
    const char c = src[i];
    dst[i]       = c;
    if (c == '\0') {
      return;
    }
  }
  dst[cap - 1U] = '\0';
}

RA_NSC_VENEER ra_err_t ra_nsc_log_emit(const char* tag, const char* message)
{
  RA_CHECK_NULL_PTR((void*)tag, s_tag, "log_emit: tag");
  RA_CHECK_NULL_PTR((void*)message, s_tag, "log_emit: message");
  /* Bound check the cap; cmse_check_address_range only validates
   * the prefix we are about to copy. */
  RA_NSC_CHECK_NS_RANGE_R(tag, (uint32_t)k_ra_nsc_log_msg_max_len);
  RA_NSC_CHECK_NS_RANGE_R(message, (uint32_t)k_ra_nsc_log_msg_max_len);
  internal_safe_strcpy(s_tag_scratch, tag, (uint32_t)k_ra_nsc_log_msg_max_len);
  internal_safe_strcpy(s_msg_scratch, message, (uint32_t)k_ra_nsc_log_msg_max_len);
  ra_log_info(s_tag_scratch, s_msg_scratch);
  return k_ra_ok;
}
