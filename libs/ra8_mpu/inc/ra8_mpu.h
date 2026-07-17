/**
 * @file ra8_mpu.h
 * @brief Cortex-M85 Memory Protection Unit (MPU) configuration helper
 * @ingroup grp_system
 *
 * @details
 * Programs the Armv8-M MPU regions inside each TrustZone world from a
 * static configuration. The MPU is independent of the SAU (which
 * partitions S vs NS); the MPU partitions memory within a single
 * world. The descriptor maps directly onto the Armv8-M RBAR / RLAR
 * fields documented in `ra8_mpu_regs.h`; this layer adds
 * power-of-two size validation, AP[1:0] encoding from human-readable
 * RO/RW/None pairs, and bounds checking against `MPU_TYPE.DREGION`.
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
 * @enum ra8_mpu_perm_t
 * @brief Per-world (priv / unpriv) access permission for an MPU region.
 *
 * @details
 * Armv8-M packs both privileged and unprivileged permissions into a
 * single AP[1:0] field; this enum surfaces the underlying RO / RW /
 * None levels so the caller specifies them independently. The Armv8-M
 * AP table cannot represent "priv RO + unpriv RW", so configure()
 * rejects that combination with `k_ra8_err_invalid_arg`.
 */
typedef enum : uint8_t {
  k_ra8_mpu_perm_none = 0U, /**< No access at this privilege level.  */
  k_ra8_mpu_perm_ro   = 1U, /**< Read-only at this privilege level.  */
  k_ra8_mpu_perm_rw   = 2U, /**< Read-write at this privilege level. */
} ra8_mpu_perm_t;

/**
 * @enum ra8_mpu_share_t
 * @brief Shareability domain for an MPU region (Armv8-M SH[1:0]).
 *
 * @details Mirrors the SH field from the Cortex-M85 TRM "MPU_RBAR".
 */
typedef enum : uint8_t {
  k_ra8_mpu_share_non   = 0U, /**< Non-shareable.   */
  k_ra8_mpu_share_outer = 2U, /**< Outer shareable. */
  k_ra8_mpu_share_inner = 3U, /**< Inner shareable. */
} ra8_mpu_share_t;

/**
 * @enum ra8_mpu_attr_idx_t
 * @brief Index into MAIR0/MAIR1 for the region's memory-attribute set.
 *
 * @details Slot 0..7. Slots 0..3 live in MAIR0; 4..7 live in MAIR1.
 */
typedef enum : uint8_t {
  k_ra8_mpu_attr_idx_0 = 0U, /**< MAIR0 byte 0. */
  k_ra8_mpu_attr_idx_1 = 1U, /**< MAIR0 byte 1. */
  k_ra8_mpu_attr_idx_2 = 2U, /**< MAIR0 byte 2. */
  k_ra8_mpu_attr_idx_3 = 3U, /**< MAIR0 byte 3. */
  k_ra8_mpu_attr_idx_4 = 4U, /**< MAIR1 byte 0. */
  k_ra8_mpu_attr_idx_5 = 5U, /**< MAIR1 byte 1. */
  k_ra8_mpu_attr_idx_6 = 6U, /**< MAIR1 byte 2. */
  k_ra8_mpu_attr_idx_7 = 7U, /**< MAIR1 byte 3. */
} ra8_mpu_attr_idx_t;

/**
 * @enum ra8_mpu_size_limits_t
 * @brief Architectural size limits for an MPU region.
 *
 * @details Armv8-M requires 32-byte alignment of base and limit.
 */
typedef enum : uint32_t {
  k_ra8_mpu_min_region_size = 32U, /**< Minimum power-of-two size in bytes. */
} ra8_mpu_size_limits_t;

/**
 * @struct ra8_mpu_region_t
 * @brief Static descriptor for one Armv8-M MPU region.
 *
 * @details
 * The driver translates this into the RBAR + RLAR pair documented in
 * the Cortex-M85 TRM "MPU register summary". `base + size` defines
 * the inclusive byte range `[base, base + size - 1]`.
 *
 * @invariant size is a power of two >= k_ra8_mpu_min_region_size.
 * @invariant (base & (size - 1)) == 0.
 */
typedef struct {
  uintptr_t          base;       /**< Region base address.                 */
  uint32_t           size;       /**< Region size in bytes (power of two). */
  ra8_mpu_perm_t     priv;       /**< Privileged-mode permission.          */
  ra8_mpu_perm_t     unpriv;     /**< Unprivileged-mode permission.        */
  bool               executable; /**< If true, instruction fetch allowed.  */
  ra8_mpu_share_t    shareable;  /**< SH[1:0] field.                       */
  ra8_mpu_attr_idx_t attr_idx;   /**< MAIR slot index.                     */
} ra8_mpu_region_t;

/**
 * @struct ra8_mpu_cfg_t
 * @brief Whole-MPU static configuration block.
 *
 * @details
 * `regions` points at a caller-owned array of `region_count` entries.
 * `mair0` / `mair1` are written verbatim into MPU_MAIR0 / MPU_MAIR1.
 *
 * @invariant region_count <= MPU_TYPE.DREGION.
 */
typedef struct {
  const ra8_mpu_region_t* regions;      /**< Region descriptor array.  */
  uint8_t                 region_count; /**< Entries in regions.       */
  uint32_t                mair0;        /**< Verbatim MPU_MAIR0 value. */
  uint32_t                mair1;        /**< Verbatim MPU_MAIR1 value. */
  bool                    privdefena;   /**< MPU_CTRL.PRIVDEFENA = 1.  */
  bool                    hfnmiena;     /**< MPU_CTRL.HFNMIENA   = 1.  */
} ra8_mpu_cfg_t;

/**
 * @brief Program every MPU region from a static configuration.
 *
 * @details
 * Disables the MPU, writes MAIR0/MAIR1, walks the region table,
 * clears any unused regions above region_count, then re-enables the
 * MPU with the requested CTRL flags.
 *
 * @param[in] cfg Configuration block.
 *
 * @return ra8_err_t error code.
 * @retval k_ra8_ok              Region table programmed and MPU enabled.
 * @retval k_ra8_err_null_ptr    cfg == NULL or cfg->regions == NULL.
 * @retval k_ra8_err_invalid_arg region_count > DREGION, or a region has
 *                              non-power-of-two size, misaligned base,
 *                              or unrepresentable AP pair.
 *
 * @pre cfg != NULL.
 * @pre Caller is in privileged mode.
 * @post On success MPU_CTRL.ENABLE == 1.
 * @post On failure no MPU state is changed.
 *
 * @note Not thread-safe.
 *
 * @see Arm Cortex-M85 TRM "MPU register summary".
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mpu_configure(const ra8_mpu_cfg_t* cfg);

/**
 * @brief Set MPU_CTRL.ENABLE.
 *
 * @return ra8_err_t error code.
 * @retval k_ra8_ok Always.
 *
 * @pre Caller is privileged.
 * @post MPU_CTRL.ENABLE == 1.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mpu_enable(void);

/**
 * @brief Clear MPU_CTRL.ENABLE.
 *
 * @return ra8_err_t error code.
 * @retval k_ra8_ok Always.
 *
 * @pre Caller is privileged.
 * @post MPU_CTRL.ENABLE == 0.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mpu_disable(void);

/**
 * @brief Program a single region without disabling the MPU.
 *
 * @param[in] region     Region index, 0..MPU_TYPE.DREGION - 1.
 * @param[in] region_cfg New descriptor.
 *
 * @return ra8_err_t error code.
 * @retval k_ra8_ok              Region updated.
 * @retval k_ra8_err_null_ptr    region_cfg == NULL.
 * @retval k_ra8_err_invalid_arg region >= DREGION, or region_cfg has
 *                              non-power-of-two size, misaligned base,
 *                              or unrepresentable AP pair.
 *
 * @pre region_cfg != NULL.
 * @pre Caller is privileged.
 * @post On success the addressed region holds the new descriptor.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mpu_set_region(uint8_t region, const ra8_mpu_region_t* region_cfg);

#ifdef __cplusplus
}
#endif
