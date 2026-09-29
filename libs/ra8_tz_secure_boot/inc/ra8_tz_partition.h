/**
 * @file ra8_tz_partition.h
 * @brief Declarative TrustZone partition for the secure boot path
 * @ingroup grp_system
 *
 * @details
 * A TrustZone partition is data, not code. Today every TrustZone app carries
 * its own `trustzone_init.c` with a private `internal_sau_set_region()`, its
 * own copy of the SAU register addresses, and four hardcoded window bases, so
 * a security-critical default-deny attribution map is maintained in five
 * places and has already drifted. This header gives that map one type: an app
 * supplies `{base, size, attr}` literals plus the SRAM boundary offsets it
 * wants, and the SAU programming, the whole-descriptor validation, and the
 * barrier placement live once.
 *
 * The SAU half is delegated to `ra8_sau.h`; what this layer adds is the
 * RA8D2-side companion state that has to move with it (the per-bank SRAM
 * Secure/Non-Secure boundary) and one atomicity rule: the whole descriptor is
 * checked before the first register write, so a rejected partition leaves the
 * device exactly as it was rather than half-attributed.
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
#include "ra8_sau.h"

/**
 * @enum ra8_tz_partition_limits_t
 * @brief Bounds a partition descriptor is checked against.
 *
 * @details
 * `k_ra8_tz_partition_sram_bank_count` mirrors the four SRAMSABARn registers
 * the RA8D2 CPSCU implements. `k_ra8_tz_partition_sram_granule` is the 4 KB
 * boundary alignment SRAMSABARn requires: the low 13 bits of the register are
 * reserved, so an offset that is not a multiple of this is a caller error
 * rather than something to round.
 */
typedef enum : uint32_t {
  k_ra8_tz_partition_sram_bank_count = 4U,      /**< SRAMSABAR0..3.               */
  k_ra8_tz_partition_sram_granule    = 0x2000U, /**< SRAMSABARn boundary granule. */
} ra8_tz_partition_limits_t;

/**
 * @struct ra8_tz_partition_t
 * @brief Whole-device security attribution map, as data.
 *
 * @details
 * `sau_regions` points at a caller-owned array of `sau_region_count`
 * descriptors. Anything not carved out stays Secure; `sau_all_ns` is the
 * explicit CTRL.ALLNS opt-in and leaving it false is the default-deny map
 * every in-tree TrustZone app actually wants.
 *
 * `sram_boundary` is optional. When it is NULL the SRAM attribution is left
 * exactly as boot ROM left it, which is what a partition that only needs the
 * SAU should say. When it is non-NULL it must carry
 * `k_ra8_tz_partition_sram_bank_count` entries, one absolute Secure-region
 * length per bank, because a partial array would leave the unnamed banks in a
 * state the descriptor does not describe.
 *
 * @invariant sau_region_count == 0 || sau_regions != NULL.
 * @invariant sram_boundary == NULL || every entry is a multiple of
 *            k_ra8_tz_partition_sram_granule.
 */
typedef struct {
  const ra8_sau_region_t* sau_regions;      /**< SAU region table.                */
  const uint32_t*         sram_boundary;    /**< Per-bank Secure length, or NULL. */
  uint8_t                 sau_region_count; /**< Entries in sau_regions.          */
  bool                    sau_all_ns;       /**< SAU_CTRL.ALLNS = 1 when true.    */
} ra8_tz_partition_t;

/**
 * @brief Check a partition descriptor without touching any register.
 *
 * @details
 * Runs the same rules `ra8_tz_partition_apply()` runs before its first write:
 * the SAU table geometry (32-byte alignment of base and size, a non-zero
 * size, no wrap past 4 GiB, a known attribute), the SAU region count against
 * the implemented SAU_TYPE.SREGION, and the SRAM boundary granularity. Exposed
 * on its own so a board file can assert its static map at bring-up, or a host
 * test can reject a map without a device.
 *
 * @param[in] partition Descriptor to check.
 *
 * @return ra8_err_t error code.
 * @retval k_ra8_ok              Descriptor is applicable as written.
 * @retval k_ra8_err_null_ptr    partition == NULL, or sau_regions == NULL with
 *                               a non-zero sau_region_count.
 * @retval k_ra8_err_invalid_arg A SAU region is misaligned, empty, wraps, or
 *                               carries an attribute outside ra8_sau_attr_t,
 *                               or an SRAM boundary offset is not a multiple
 *                               of k_ra8_tz_partition_sram_granule.
 * @retval k_ra8_err_not_supported sau_region_count exceeds the regions this
 *                               SAU implements.
 *
 * @pre partition != NULL.
 * @post No device state is changed on any path.
 *
 * @note Not thread-safe; boot-path helper.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_tz_partition_validate(const ra8_tz_partition_t* partition);

/**
 * @brief Apply a partition: programme the SAU, then the SRAM boundaries.
 *
 * @details
 * Validates the whole descriptor first, so a rejected partition leaves the
 * device untouched and a caller cannot end up with the SAU enabled over an
 * SRAM split that was refused. The SAU goes first because it is the coarse
 * map the SRAM boundary refines, and it is programmed through
 * `ra8_sau_configure()`, which clears every region above `sau_region_count`;
 * a reconfigure therefore cannot inherit an enabled window from the boot ROM
 * or from an earlier partition.
 *
 * @param[in] partition Descriptor to apply.
 *
 * @return ra8_err_t error code.
 * @retval k_ra8_ok              SAU enabled and, when requested, the four SRAM
 *                               boundaries written.
 * @retval k_ra8_err_null_ptr    As ra8_tz_partition_validate().
 * @retval k_ra8_err_invalid_arg As ra8_tz_partition_validate().
 * @retval k_ra8_err_not_supported As ra8_tz_partition_validate().
 *
 * @pre partition != NULL.
 * @pre Caller is in Secure privileged mode, before any Non-Secure code runs.
 * @pre PRCR_S.PRC4 is unlocked when `sram_boundary` is non-NULL.
 * @post On success SAU_CTRL.ENABLE == 1.
 * @post On a validation failure no register is written.
 *
 * @note Not thread-safe; runs once at boot.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_tz_partition_apply(const ra8_tz_partition_t* partition);

/**
 * @brief The validated EK-RA8D2 partition, as data.
 *
 * @details
 * The four windows every in-tree `trustzone_init.c` hardcodes: Non-Secure
 * upper MRAM, Non-Secure upper SRAM, Non-Secure upper SDRAM, and the NSC
 * veneer alias the linker fills through `.gnu.sgstubs`. Lower MRAM and lower
 * SRAM are deliberately absent, and that absence is what keeps the secure
 * image and the key vault Secure.
 *
 * Returned by pointer to static storage so a board file can apply it, extend
 * a copy of it, or assert against it without owning the numbers.
 *
 * @return Pointer to the canonical descriptor; never NULL.
 *
 * @post The returned descriptor names k_ra8_sau_boot_region_count regions and
 *       leaves `sram_boundary` NULL.
 *
 * @note Thread safety: read-only static data.
 * @since 0.1.0
 */
[[nodiscard]] const ra8_tz_partition_t* ra8_tz_partition_board_map(void);

#ifdef __cplusplus
}
#endif
