/**
 * @file ra8_nsc_log.c
 * @brief NSC veneer: secure-side logging
 *
 * @par Tag
 * [Ring 4 / NSC] {World: NSC}
 *
 * @details
 * scaffold. Forwards the (tag, message) pair to
 * ``ra8_log_info`` after copying both strings into a small
 * secure-side scratch area to make sure the secure code never
 * dereferences NS pointers directly.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_log.h"
#include "ra8_nsc.h"
#include "ra8_nsc_veneer.h"

static const char* s_tag = "NSCLOG";

/**
 * @var s_tag_scratch
 * @brief Secure scratch for the (tag, message) copy.
 *
 * @details
 * Living in the secure data section, this buffer is updated
 * before each ra8_log_info call so the secure code never has to
 * read from a Non-Secure pointer.
 */
static char s_tag_scratch[k_ra8_nsc_log_msg_max_len];
static char s_msg_scratch[k_ra8_nsc_log_msg_max_len];

/**
 * @brief Length-bounded string copy with NUL terminator.
 *
 * @details Copies up to ``cap-1`` bytes from ``src`` into ``dst`` and
 *   always writes a terminating NUL.
 *
 * @param[out] dst Destination buffer (must hold ``cap`` bytes).
 * @param[in]  src Source NUL-terminated string.
 * @param[in]  cap Capacity of ``dst`` in bytes; must be >= 1.
 *
 * @pre ``dst`` and ``src`` non-NULL.
 * @pre ``cap`` >= 1.
 * @post ``dst`` is NUL-terminated.
 * @post At most ``cap-1`` source bytes are copied.
 *
 * @note Static helper; not thread-safe.
 * @since 0.1.0
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

/**
 * @brief NSC veneer: emit a log line from Non-Secure code.
 *
 * @details Copies (tag,message) into secure scratch buffers and forwards
 *   to ``ra8_log_info``.
 *
 * @param[in] tag     NUL-terminated NS string (subsystem tag).
 * @param[in] message NUL-terminated NS string (log message body).
 *
 * @return ra8_err_t outcome.
 * @retval k_ra8_ok            Message handed to the secure logger.
 * @retval k_ra8_err_null_ptr  A pointer argument was NULL.
 *
 * @pre TrustZone substrate has been initialized.
 * @pre Both strings reside in the NS data region.
 * @post Tag/message copied into secure scratch and forwarded.
 * @post Strings truncated to ``k_ra8_nsc_log_msg_max_len-1`` if longer.
 *
 * @note Thread-safe: no; the secure scratch is shared.
 * @since 0.1.0
 */
RA8_NSC_VENEER ra8_err_t ra8_nsc_log_emit(const char* tag, const char* message)
{
  RA8_CHECK_NULL_PTR(tag, s_tag, "log_emit: tag");
  RA8_CHECK_NULL_PTR(message, s_tag, "log_emit: message");
  /* Bound check the cap; cmse_check_address_range only validates
   * the prefix we are about to copy. */
  RA8_NSC_CHECK_NS_RANGE_R(tag, (uint32_t)k_ra8_nsc_log_msg_max_len);
  RA8_NSC_CHECK_NS_RANGE_R(message, (uint32_t)k_ra8_nsc_log_msg_max_len);
  internal_safe_strcpy(s_tag_scratch, tag, k_ra8_nsc_log_msg_max_len);
  internal_safe_strcpy(s_msg_scratch, message, k_ra8_nsc_log_msg_max_len);
  ra8_log_info(s_tag_scratch, s_msg_scratch);
  return k_ra8_ok;
}
