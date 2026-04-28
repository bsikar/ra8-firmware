/**
 * @file ra_nsc_veneer.h
 * @brief Macros for declaring Non-Secure Callable veneer functions
 *
 * @par Tag
 * [Ring 4 / NSC] {World: NSC}
 *
 * @details
 * enables ``__attribute__((cmse_nonsecure_entry))`` on the
 * NSC veneer functions when the firmware is built with
 * ``-DRA_TRUSTZONE_ENABLE=ON``. In single-world builds the macros
 * compile to nothing, so the same veneer source compiles cleanly
 * against either build configuration.
 *
 * Use ``RA_NSC_VENEER`` on the function declarations in
 * ``ra_nsc.h`` and on the matching definitions in
 * ``libs/ra_nsc/src``. Use ``RA_NSC_CHECK_NS_RANGE_R`` /
 * ``_RW`` to validate that pointer arguments lie inside the
 * Non-Secure region; the macros expand to
 * ``cmse_check_address_range`` calls under TrustZone and to
 * no-ops otherwise.
 *
 * ## Argument constraints
 *
 * ``cmse_nonsecure_entry`` imposes restrictions: parameters must
 * be in registers (no struct-by-value bigger than a register),
 * and the return value must fit in registers as well. The
 * existing veneer signatures already comply -- everything is
 * ``uint*_t`` / pointer / enum.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#ifdef RA_TRUSTZONE_ENABLE

#include <arm_cmse.h>

/**
 * @def RA_NSC_VENEER
 * @brief Marks a function as a Non-Secure Callable entry point.
 */
#define RA_NSC_VENEER __attribute__((cmse_nonsecure_entry))

/**
 * @def RA_NSC_CHECK_NS_RANGE_R(ptr, len)
 * @brief Validate that ``[ptr, ptr+len)`` is readable from NS code.
 *
 * @details
 * Returns ``k_ra_err_invalid_arg`` from the surrounding function
 * if any byte in the range is in the secure region or otherwise
 * inaccessible to NS callers.
 */
#define RA_NSC_CHECK_NS_RANGE_R(ptr, len)                                                          \
  do {                                                                                             \
    if (cmse_check_address_range((void*)(ptr), (uint32_t)(len), CMSE_NONSECURE | CMSE_MPU_READ) == \
        nullptr) {                                                                                 \
      return k_ra_err_invalid_arg;                                                                 \
    }                                                                                              \
  } while (0)

/**
 * @def RA_NSC_CHECK_NS_RANGE_RW(ptr, len)
 * @brief Validate that ``[ptr, ptr+len)`` is read/write from NS code.
 */
#define RA_NSC_CHECK_NS_RANGE_RW(ptr, len)                                                         \
  do {                                                                                             \
    if (cmse_check_address_range((void*)(ptr),                                                     \
                                 (uint32_t)(len),                                                  \
                                 CMSE_NONSECURE | CMSE_MPU_READWRITE) == nullptr) {                \
      return k_ra_err_invalid_arg;                                                                 \
    }                                                                                              \
  } while (0)

#else /* !RA_TRUSTZONE_ENABLE */

#define RA_NSC_VENEER
#define RA_NSC_CHECK_NS_RANGE_R(ptr, len)  ((void)(ptr), (void)(len))
#define RA_NSC_CHECK_NS_RANGE_RW(ptr, len) ((void)(ptr), (void)(len))

#endif /* RA_TRUSTZONE_ENABLE */

#ifdef __cplusplus
}
#endif
