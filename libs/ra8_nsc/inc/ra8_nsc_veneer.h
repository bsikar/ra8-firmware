/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_nsc_veneer.h
 * @brief Macros for declaring Non-Secure Callable veneer functions
 * @ingroup grp_security
 *
 * @par Tag
 * [Ring 4 / NSC] {World: NSC}
 *
 * @details
 * enables ``__attribute__((cmse_nonsecure_entry))`` on the
 * NSC veneer functions when the firmware is built with
 * ``-DRA8_TRUSTZONE_ENABLE=ON`` **and** the translation unit is
 * compiled for the Secure world (``-mcmse``, i.e.
 * ``__ARM_FEATURE_CMSE`` bit 1 set). Non-Secure images define
 * ``RA8_TRUSTZONE_ENABLE`` too but compile without ``-mcmse``; they
 * must see the veneers as plain function declarations, because gcc
 * ignores ``cmse_nonsecure_entry`` without ``-mcmse`` and the
 * -Wattributes diagnostic is fatal under the project -Werror
 * profile. In single-world and host builds the macros likewise
 * compile to nothing, so the same veneer source compiles cleanly
 * against every build configuration.
 *
 * Use ``RA8_NSC_VENEER`` on the function declarations in
 * ``ra8_nsc.h`` and on the matching definitions in
 * ``libs/ra8_nsc/src``. Use ``RA8_NSC_CHECK_NS_RANGE_R`` /
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
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_attributes.h"

/* ra8_attributes.h carries a generic, annotation-only RA8_NSC_VENEER so that
 * translation units which never touch the NSC boundary can still be scanned by
 * the libclang annotation gate. This header is the single authority for the
 * macro: the Secure-world form must also carry
 * __attribute__((cmse_nonsecure_entry)), which is what actually emits the
 * secure-gateway (SG) veneer. Undefining first makes this file win regardless
 * of include order -- without it, a TU that reached ra8_attributes.h after this
 * header would silently replace the cmse form with the annotation-only form and
 * drop the SG veneer, breaking the S/NS boundary with no diagnostic beyond a
 * macro-redefinition warning. Both branches below re-attach the annotation so
 * the gate still sees every veneer. */
#undef RA8_NSC_VENEER

/* __ARM_FEATURE_CMSE bit 1 marks a Secure-world (-mcmse) compile: only there
 * is cmse_nonsecure_entry meaningful. NS TUs share RA8_TRUSTZONE_ENABLE but
 * compile without -mcmse and must take the plain-declaration branch. */
#if defined(RA8_TRUSTZONE_ENABLE) && defined(__ARM_FEATURE_CMSE) && ((__ARM_FEATURE_CMSE & 2) != 0)

#include <arm_cmse.h>

/**
 * @def RA8_NSC_VENEER
 * @brief Marks a function as a Non-Secure Callable entry point.
 *
 * @details
 * Secure-world form: carries both the architectural
 * ``cmse_nonsecure_entry`` attribute (which emits the secure-gateway
 * veneer) and the ``ra8_nsc_veneer`` annotation consumed by
 * ``scripts/checks/check_annotations.py``. The annotation is written
 * first because ``[[clang::annotate]]`` is a C23 attribute and must
 * precede the declaration specifiers; the GNU-style attribute follows.
 */
#define RA8_NSC_VENEER RA8_INTERNAL_ANNOTATE("ra8_nsc_veneer") __attribute__((cmse_nonsecure_entry))

/**
 * @def RA8_NSC_CHECK_NS_RANGE_R(ptr, len)
 * @brief Validate that ``[ptr, ptr+len)`` is readable from NS code.
 *
 * @details
 * Returns ``k_ra8_err_invalid_arg`` from the surrounding function
 * if any byte in the range is in the secure region or otherwise
 * inaccessible to NS callers.
 */
#define RA8_NSC_CHECK_NS_RANGE_R(ptr, len)                                                         \
  do {                                                                                             \
    if (cmse_check_address_range((void*)(uintptr_t)(ptr),                                          \
                                 (uint32_t)(len),                                                  \
                                 CMSE_NONSECURE | CMSE_MPU_READ) == nullptr) {                     \
      return k_ra8_err_invalid_arg;                                                                \
    }                                                                                              \
  } while (0)

/**
 * @def RA8_NSC_CHECK_NS_RANGE_RW(ptr, len)
 * @brief Validate that ``[ptr, ptr+len)`` is read/write from NS code.
 */
#define RA8_NSC_CHECK_NS_RANGE_RW(ptr, len)                                                        \
  do {                                                                                             \
    if (cmse_check_address_range((void*)(uintptr_t)(ptr),                                          \
                                 (uint32_t)(len),                                                  \
                                 CMSE_NONSECURE | CMSE_MPU_READWRITE) == nullptr) {                \
      return k_ra8_err_invalid_arg;                                                                \
    }                                                                                              \
  } while (0)

#else /* !RA8_TRUSTZONE_ENABLE or a non-Secure-world compile */

/** @brief RA8 NSC VENEER -- annotation only: no -mcmse, so no SG veneer. */
#define RA8_NSC_VENEER                      RA8_INTERNAL_ANNOTATE("ra8_nsc_veneer")
/** @brief RA8 NSC CHECK NS RANGE r. */
#define RA8_NSC_CHECK_NS_RANGE_R(ptr, len)  ((void)(ptr), (void)(len))
/** @brief RA8 NSC CHECK NS RANGE RW. */
#define RA8_NSC_CHECK_NS_RANGE_RW(ptr, len) ((void)(ptr), (void)(len))

#endif /* RA8_TRUSTZONE_ENABLE && Secure-world (-mcmse) compile */

#ifdef __cplusplus
}
#endif
