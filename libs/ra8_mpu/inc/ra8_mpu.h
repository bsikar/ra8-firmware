/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
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
 * @enum ra8_mpu_boot_layout_t
 * @brief Fixed geometry of the canonical boot memory-attribute map.
 *
 * @details
 * `ra8_mpu_apply_boot_map()` installs exactly this many regions. The count is
 * a contract shared by the boot caller (`SystemInit()`) and the host tests, so
 * it lives in the header rather than being buried in the implementation.
 */
typedef enum : uint8_t {
  k_ra8_mpu_boot_region_count = 5U, /**< Regions in the canonical boot map. */
} ra8_mpu_boot_layout_t;

/**
 * @enum ra8_mpu_boot_mair_t
 * @brief MAIR0/MAIR1 attribute-indirection words for the canonical boot map.
 *
 * @details
 * Three memory-attribute sets are used by the boot map, packed into MAIR0:
 * - byte 0 (AttrIdx 0) = 0xFF: Normal, inner+outer write-back / write-allocate.
 * - byte 1 (AttrIdx 1) = 0x44: Normal, inner+outer non-cacheable.
 * - byte 2 (AttrIdx 2) = 0x04: Device-nGnRE.
 * MAIR1 is unused because the boot map has no region with AttrIdx >= 4. See the
 * Armv8-M ARM "MAIR0/MAIR1, MPU Memory Attribute Indirection Registers".
 *
 * @see ra8_mpu_apply_boot_map()
 */
typedef enum : uint32_t {
  k_ra8_mpu_boot_mair0 = 0x000444FFUL, /**< Boot MAIR0: WB/WA, non-cacheable, Device. */
  k_ra8_mpu_boot_mair1 = 0x00000000UL, /**< Boot MAIR1: unused (no AttrIdx >= 4).     */
} ra8_mpu_boot_mair_t;

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

/**
 * @brief Install the canonical 5-region boot memory-attribute map and enable the MPU.
 *
 * @details
 * Programs the fixed attribute map every RA8D2 image needs out of reset --
 * RO+executable cacheable MRAM code, RW/XN cacheable M85-private SRAM and
 * SDRAM, RW/XN Device-nGnRE peripherals, and RW/XN Normal-non-cacheable shared
 * M85<->M33 SRAM -- then enables the MPU with PRIVDEFENA so anything the map
 * does not cover keeps the privileged default memory map. This is the
 * boot-usable entry point that replaces the hand-rolled MAIR/RBAR/RLAR/CTRL
 * pokes each app's `system_init.c` used to duplicate: the boot path routes its
 * attribute map through this one driver exactly as it already calls
 * `ra8_cache_dcache_invalidate_all()`.
 *
 * Callable from `SystemInit()` before `.data`/`.bss` are initialised: it reads
 * only the driver-owned `const` region table (in `.rodata`) and MMIO, writes no
 * `.data`/`.bss`, and never logs. The shared M85<->M33 bank is 640 KiB, which
 * is not a power of two and so cannot be expressed through the size-checked
 * `ra8_mpu_set_region()`; this entry point encodes it by base+limit directly,
 * which the Armv8-M PMSAv8 RBAR/RLAR pair supports natively.
 *
 * @return ra8_err_t error code.
 * @retval k_ra8_ok              Map installed; MPU enabled with PRIVDEFENA.
 * @retval k_ra8_err_invalid_arg MPU_TYPE.DREGION reports fewer than
 *                              `k_ra8_mpu_boot_region_count` implemented
 *                              regions; the MPU is left disabled.
 *
 * @pre Caller is privileged (true out of reset in the Secure state).
 * @pre The core MPU is idle (freshly reset or previously disabled).
 * @post On k_ra8_ok, MPU_CTRL.ENABLE == 1 and MPU_CTRL.PRIVDEFENA == 1.
 * @post On failure the MPU is left disabled; no partial map is enabled.
 *
 * @note Not thread-safe; single-threaded boot / init context only.
 * @note Does not touch SHCSR.MEMFAULTENA -- the boot path enables the
 *       configurable-fault handlers separately; a runtime caller wanting
 *       MemManage delivery uses `ra8_mpu_configure()` instead.
 *
 * @see ra8_mpu_boot_map()  Inspect the region table this installs.
 * @see ra8_mpu_configure()  Full runtime configuration from a caller table.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mpu_apply_boot_map(void);

/**
 * @brief Return the canonical boot memory-attribute map region table.
 *
 * @details
 * Read-only view of the exact region descriptors `ra8_mpu_apply_boot_map()`
 * installs, so callers (a boot self-test, the host unit tests) can inspect or
 * cross-check the map without re-encoding it. The pointer targets a
 * driver-owned `static const` table in `.rodata`; the entries are immutable.
 *
 * @param[out] out_count Receives the region count
 *                       (`k_ra8_mpu_boot_region_count`). Must be non-NULL.
 *
 * @return Pointer to the first of `*out_count` region descriptors, or NULL when
 *         @p out_count is NULL.
 * @retval NULL @p out_count was NULL; `*out_count` is not written.
 *
 * @pre @p out_count != NULL.
 * @post On non-NULL return, `*out_count == k_ra8_mpu_boot_region_count`.
 * @post The referenced table is not modified.
 *
 * @note Thread-safe: returns a pointer to immutable data.
 * @see ra8_mpu_apply_boot_map()
 * @since 0.1.0
 */
const ra8_mpu_region_t* ra8_mpu_boot_map(uint8_t* out_count);

/**
 * @brief Report whether the core MPU is currently enabled.
 *
 * @details
 * Reads MPU_CTRL.ENABLE (Armv8-M "MPU_CTRL"). Lets application code confirm the
 * boot attribute map came up without poking the register block directly -- the
 * whole point of routing MPU access through this driver.
 *
 * @return Whether MPU_CTRL.ENABLE is set.
 * @retval true  The MPU is enabled.
 * @retval false The MPU is disabled.
 *
 * @pre Caller is privileged (or RA8_OFF_TARGET, where the register is faked).
 * @post No MPU state is modified (pure read).
 *
 * @note Not thread-safe with respect to a concurrent enable/disable.
 * @see ra8_mpu_enable()
 * @see ra8_mpu_apply_boot_map()
 * @since 0.1.0
 */
[[nodiscard]] bool ra8_mpu_is_enabled(void);

#ifdef __cplusplus
}
#endif
