/**
 * @file ra_ble_patch.c
 * @brief Renesas BLE controller firmware patch loader -- stub implementation.
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * The encrypted BLE patch image is distributed by Renesas under NDA
 * and cannot be redistributed in this open-source repository, so
 * this TU is a stub-with-warning. See ra_ble_patch.h for the full
 * contract; this file only restates the runtime behaviour:
 *
 *   - On every call we log a single one-shot warning so the firmware
 *     bring-up logs make it obvious that the radio will not actually
 *     transmit until the user replaces this stub with a real loader.
 *   - We track whether a patch was ever "successfully" loaded -- in
 *     stub mode that flag is always 0.
 *
 * Wired into the BLE init path so the radio bring-up code path is
 * complete; ``ra_ble_open`` (or its rework) calls
 * ``ra_ble_patch_load(NULL, 0)`` early and proceeds based on the
 * return value. This keeps the radio code path linear and exercised
 * by every CI build.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_ble_patch.h"

#include <stddef.h>
#include <stdint.h>

#include "ra_err.h"
#include "ra_log.h"

/* ============================================================ */
/* Internal state */
/* ============================================================ */

/**
 * @var s_patch_loaded
 * @brief 1 if a real patch loader populated the controller; 0 in stub mode.
 *
 * @details
 * Mutated only by ``ra_ble_patch_load`` and read by
 * ``ra_ble_patch_is_loaded``. Single-threaded init-time access.
 *
 * @since 0.1.0
 */
static uint8_t s_patch_loaded;

/**
 * @var s_warned
 * @brief 1 once the "no patch image" warning has been emitted.
 *
 * @details
 * Suppresses repeated log noise when the BLE init path retries the
 * load (for example after a controller reset).
 *
 * @since 0.1.0
 */
static uint8_t s_warned;

/**
 * @brief Tag used for ra_log_warn output from this module.
 *
 * @since 0.1.0
 */
static const char* s_ra_ble_patch_tag = "ble_patch";

/* ============================================================ */
/* Public API */
/* ============================================================ */

ra_err_t ra_ble_patch_load(const uint8_t* image, size_t len)
{
  /* "Probe" path: caller asks if a patch is configured. */
  if (image == nullptr && len == 0U) {
    if (s_warned == 0U) {
      ra_log_warn(s_ra_ble_patch_tag,
                  "no BLE patch image configured -- radio TX/RX will not work "
                  "until a Renesas-supplied patch is linked in");
      s_warned = 1U;
    }
    s_patch_loaded = 0U;
    return k_ra_err_not_supported;
  }

  if (image == nullptr) {
    return k_ra_err_null_ptr;
  }

  if (len < (size_t)k_ra_ble_patch_min_bytes || len > (size_t)k_ra_ble_patch_max_bytes) {
    return k_ra_err_invalid_arg;
  }

  /*
   * Stub: a real loader would stream `image` into PATCHADDR /
   * PATCHDATA, wait for STATUS.patch_done, then set s_patch_loaded.
   * Until that loader is linked in, fail loud.
   */
  ra_log_warn(s_ra_ble_patch_tag,
              "ra_ble_patch_load called with an image but no production "
              "loader is linked in -- replace this TU before deployment");
  s_patch_loaded = 0U;
  return k_ra_err_not_supported;
}

uint8_t ra_ble_patch_is_loaded(void)
{
  return s_patch_loaded;
}

#ifdef UNIT_TEST
/* Test-hook prototypes (external linkage; production firmware ignores). */
void    ra_ble_patch_test_reset(void);
uint8_t ra_ble_patch_test_warned(void);

/**
 * @brief Test hook -- reset the file-scope flags between cases.
 *
 * @since 0.1.0
 *
 * @details See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 */
void ra_ble_patch_test_reset(void)
{
  s_patch_loaded = 0U;
  s_warned       = 0U;
}

/**
 * @brief Test hook -- read the warned flag.
 *
 * @since 0.1.0
 *
 * @details See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 */
uint8_t ra_ble_patch_test_warned(void)
{
  return s_warned;
}
#endif
