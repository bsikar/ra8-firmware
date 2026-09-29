/**
 * @file ra8_sau.h
 * @brief Armv8-M Security Attribution Unit (SAU) partition helper
 * @ingroup grp_system
 *
 * @details
 * Programs the Armv8-M SAU from a declarative partition descriptor, the same
 * shape `ra8_mpu.h` already gives the MPU. The SAU decides which addresses are
 * Secure, Non-Secure, or Non-Secure Callable; the MPU partitions memory inside
 * one world. Callers declare `{base, size, attr}` and this layer derives the
 * RBAR base, the `base + size - 32` RLAR limit, and the ENABLE / NSC bits, so a
 * limit computed one region short cannot silently leave secure memory
 * NS-accessible.
 *
 * Anything the table does not carve out stays Secure: `all_ns` is the explicit
 * opt-in to CTRL.ALLNS, and leaving it false is the default-deny partition
 * every in-tree `trustzone_init.c` already wants.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"

/**
 * @enum ra8_sau_attr_t
 * @brief Security attribute a SAU region publishes for its address range.
 *
 * @details
 * Armv8-M encodes the choice as RLAR.NSC beside RLAR.ENABLE; this enum names
 * the intent instead. There is no `secure` member on purpose: Secure is the
 * absence of a region, not a region you place.
 */
typedef enum : uint8_t {
  k_ra8_sau_attr_ns  = 0U, /**< Non-Secure range.          */
  k_ra8_sau_attr_nsc = 1U, /**< Non-Secure Callable range. */
} ra8_sau_attr_t;

/**
 * @enum ra8_sau_size_limits_t
 * @brief Architectural granularity of a SAU region.
 *
 * @details Armv8-M requires 32-byte alignment of both base and limit.
 */
typedef enum : uint32_t {
  k_ra8_sau_region_granule = 32U, /**< Alignment and minimum size in bytes. */
} ra8_sau_size_limits_t;

/**
 * @struct ra8_sau_region_t
 * @brief Static descriptor for one SAU region.
 *
 * @details
 * Covers the inclusive byte range `[base, base + size - 1]`. Unlike the MPU,
 * the SAU does not require a power-of-two size: RBAR / RLAR are an address
 * pair, so any 32-byte-aligned window is representable.
 *
 * @invariant (base & (k_ra8_sau_region_granule - 1)) == 0.
 * @invariant (size & (k_ra8_sau_region_granule - 1)) == 0.
 * @invariant size >= k_ra8_sau_region_granule.
 */
typedef struct {
  uintptr_t      base; /**< Region base address, 32-byte aligned.   */
  uint32_t       size; /**< Region size in bytes, 32-byte multiple. */
  ra8_sau_attr_t attr; /**< NS or NSC.                              */
} ra8_sau_region_t;

/**
 * @struct ra8_sau_cfg_t
 * @brief Whole-SAU static partition descriptor.
 *
 * @details
 * `regions` points at a caller-owned array of `region_count` entries. Regions
 * above `region_count` are cleared, so a configure() call fully describes the
 * partition rather than layering onto whatever the boot ROM left behind.
 *
 * @invariant region_count <= SAU_TYPE.SREGION.
 */
typedef struct {
  const ra8_sau_region_t* regions;      /**< Region descriptor array.      */
  uint8_t                 region_count; /**< Entries in regions.           */
  bool                    all_ns;       /**< SAU_CTRL.ALLNS = 1 when true. */
} ra8_sau_cfg_t;

/**
 * @enum ra8_sau_boot_layout_t
 * @brief Fixed geometry of the canonical boot partition.
 *
 * @details
 * `ra8_sau_boot_map()` describes exactly this many regions. The count is a
 * contract shared by the boot caller (`ra8_trustzone_init()`) and the host
 * tests, so it lives in the header rather than the implementation.
 */
typedef enum : uint8_t {
  k_ra8_sau_boot_region_count = 4U, /**< Regions in the canonical boot partition. */
} ra8_sau_boot_layout_t;

/**
 * @brief Program the whole SAU from a static partition descriptor.
 *
 * @details
 * Disables the SAU, walks the region table writing RNR / RBAR / RLAR, clears
 * every region above `region_count`, then re-enables it with the requested
 * ALLNS setting. Validation runs over the entire table before the first
 * register write, so a rejected descriptor leaves the SAU untouched.
 *
 * @param[in] cfg Partition descriptor.
 *
 * @return ra8_err_t error code.
 * @retval k_ra8_ok              Partition programmed and SAU enabled.
 * @retval k_ra8_err_null_ptr    cfg == NULL, or cfg->regions == NULL with a
 *                              non-zero region_count.
 * @retval k_ra8_err_invalid_arg region_count > SAU_TYPE.SREGION, or a region
 *                              has a misaligned base, a size that is not a
 *                              non-zero multiple of the 32-byte granule, a
 *                              range that wraps past 4 GiB, or an attr outside
 *                              ra8_sau_attr_t.
 *
 * @pre cfg != NULL.
 * @pre Caller is in secure privileged mode.
 * @post On success SAU_CTRL.ENABLE == 1.
 * @post On failure no SAU state is changed.
 *
 * @note Not thread-safe.
 *
 * @see Armv8-M ARM "SAU registers".
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sau_configure(const ra8_sau_cfg_t* cfg);

/**
 * @brief Program a single SAU region without disabling the unit.
 *
 * @param[in] region     Region index, 0..SAU_TYPE.SREGION - 1.
 * @param[in] region_cfg New descriptor.
 *
 * @return ra8_err_t error code.
 * @retval k_ra8_ok              Region updated and enabled.
 * @retval k_ra8_err_null_ptr    region_cfg == NULL.
 * @retval k_ra8_err_invalid_arg region >= SAU_TYPE.SREGION, or region_cfg
 *                              fails the alignment / size / attr checks.
 *
 * @pre region_cfg != NULL.
 * @pre Caller is in secure privileged mode.
 * @post On success the addressed region holds the new descriptor.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sau_set_region(uint8_t region, const ra8_sau_region_t* region_cfg);

/**
 * @brief Set SAU_CTRL.ENABLE, preserving ALLNS.
 *
 * @return ra8_err_t error code.
 * @retval k_ra8_ok Always.
 *
 * @pre Caller is in secure privileged mode.
 * @post SAU_CTRL.ENABLE == 1.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sau_enable(void);

/**
 * @brief Clear SAU_CTRL.ENABLE, preserving ALLNS.
 *
 * @return ra8_err_t error code.
 * @retval k_ra8_ok Always.
 *
 * @pre Caller is in secure privileged mode.
 * @post SAU_CTRL.ENABLE == 0.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sau_disable(void);

/**
 * @brief Report whether the SAU is currently enabled.
 *
 * @return Whether SAU_CTRL.ENABLE is set.
 * @retval true  SAU is attributing addresses.
 * @retval false SAU is off; the IDAU alone decides.
 *
 * @pre Caller is in secure privileged mode.
 * @post No SAU state is changed.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] bool ra8_sau_is_enabled(void);

/**
 * @brief Report the number of SAU regions the silicon implements.
 *
 * @return SAU_TYPE.SREGION.
 *
 * @pre Caller is in secure privileged mode.
 * @post No SAU state is changed.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] uint8_t ra8_sau_region_count(void);

/**
 * @brief Borrow the canonical boot partition descriptor.
 *
 * @details
 * The four-region default-deny partition both board libraries program out of
 * reset: NS upper MRAM, NS upper SRAM, NS upper SDRAM, and the NSC veneer
 * alias the linker fills through `.gnu.sgstubs`. Lower MRAM and lower SRAM are
 * absent from the table on purpose, which is what keeps the secure image and
 * the key vault Secure.
 *
 * Returns driver-owned `const` storage in `.rodata`, so it is readable before
 * `.data` / `.bss` are initialised.
 *
 * @return Pointer to the canonical boot descriptor. Never NULL.
 *
 * @pre None.
 * @post No SAU state is changed.
 *
 * @note Not thread-safe.
 * @see ra8_sau_apply_boot_map()
 * @since 0.1.0
 */
[[nodiscard]] const ra8_sau_cfg_t* ra8_sau_boot_map(void);

/**
 * @brief Install the canonical boot partition and enable the SAU.
 *
 * @details
 * The boot-usable entry point that replaces the hand-rolled RNR / RBAR / RLAR
 * pokes each `trustzone_init.c` duplicates. Callable from the reset path
 * before `.data` / `.bss` are initialised: it reads only the driver-owned
 * `const` table and MMIO, writes no `.data` / `.bss`, and never logs.
 *
 * @return ra8_err_t error code.
 * @retval k_ra8_ok              Partition installed; SAU enabled.
 * @retval k_ra8_err_invalid_arg Silicon reports fewer regions than the boot
 *                              partition needs, so nothing was programmed.
 *
 * @pre Caller is in secure privileged mode.
 * @post On success SAU_CTRL.ENABLE == 1 and ALLNS == 0.
 * @post On failure no SAU state is changed.
 *
 * @note Not thread-safe.
 * @see ra8_sau_boot_map()
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_sau_apply_boot_map(void);

#ifdef __cplusplus
}
#endif
