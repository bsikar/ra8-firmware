/**
 * @file ra8_tz_ipc_attr.c
 * @brief Declarative IPC attribution: encoding and the typed security_init
 *
 * @par Tag
 * [Ring 1 / Boot] {World: S}
 *
 * @details
 * Implements the `ra8_tz_ipc_attr.h` contract. The bit layout of IPCSAR and
 * IPCPAR lives here and nowhere else in this library: callers name targets and
 * this TU turns them into the two CPSCU words. The register write itself is
 * still `ra8_tz_secure_boot_security_init()`, so the PRCR_S unlock / relock
 * sequence has exactly one implementation.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_tz_ipc_attr.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_tz_secure_boot.h"

/**
 * @enum ra8_tz_ipc_attr_shift_t
 * @brief Bit position of each target within IPCSAR / IPCPAR.
 *
 * @details
 * HUM Ch 3.2.1 "IPCSAR" p 205-207 places SAIPCSEM0/1 at bits 0/1, SAIPCNMI0/1
 * at bits 8/9 and SAIPCIR0..3 at bits 16..19. HUM Ch 3.2.2 "IPCPAR" p 208-209
 * mirrors every one of those positions, which is why one table serves both
 * words and why the two registers are so easy to confuse for each other.
 */
typedef enum : uint8_t {
  k_ra8_tz_ipc_shift_sem_low   = 0U,  /**< SAIPCSEM0 / PAIPCSEM0. */
  k_ra8_tz_ipc_shift_sem_high  = 1U,  /**< SAIPCSEM1 / PAIPCSEM1. */
  k_ra8_tz_ipc_shift_nmi_unit0 = 8U,  /**< SAIPCNMI0 / PAIPCNMI0. */
  k_ra8_tz_ipc_shift_nmi_unit1 = 9U,  /**< SAIPCNMI1 / PAIPCNMI1. */
  k_ra8_tz_ipc_shift_channel0  = 16U, /**< SAIPCIR0 / PAIPCIR0.   */
  k_ra8_tz_ipc_shift_channel1  = 17U, /**< SAIPCIR1 / PAIPCIR1.   */
  k_ra8_tz_ipc_shift_channel2  = 18U, /**< SAIPCIR2 / PAIPCIR2.   */
  k_ra8_tz_ipc_shift_channel3  = 19U, /**< SAIPCIR3 / PAIPCIR3.   */
} ra8_tz_ipc_attr_shift_t;

/**
 * @var s_target_shift
 * @brief Bit position per ::ra8_tz_ipc_target_t, in target order.
 *
 * @details
 * Indexed by the enum, so the table and the enum cannot drift apart without
 * the count assertion below failing to compile.
 */
static const uint8_t s_target_shift[k_ra8_tz_ipc_target_count] = {
  (uint8_t)k_ra8_tz_ipc_shift_sem_low,   (uint8_t)k_ra8_tz_ipc_shift_sem_high,
  (uint8_t)k_ra8_tz_ipc_shift_nmi_unit0, (uint8_t)k_ra8_tz_ipc_shift_nmi_unit1,
  (uint8_t)k_ra8_tz_ipc_shift_channel0,  (uint8_t)k_ra8_tz_ipc_shift_channel1,
  (uint8_t)k_ra8_tz_ipc_shift_channel2,  (uint8_t)k_ra8_tz_ipc_shift_channel3,
};

static_assert(sizeof(s_target_shift) == (unsigned)k_ra8_tz_ipc_target_count,
              "one shift per IPC target");

/**
 * @brief Set one bit of `word` when `flag` is 1.
 *
 * @details
 * Shared by both words because the positions are shared; the caller supplies
 * the already-validated 0/1 the descriptor's enum carries.
 *
 * @param[in] word   Accumulated register value.
 * @param[in] shift  Bit position from ::s_target_shift.
 * @param[in] flag   1 to set the bit, 0 to leave it clear.
 *
 * @return uint32_t `word` with the bit applied.
 *
 * @note Thread-safe (pure).
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_apply(uint32_t word, uint8_t shift, uint8_t flag)
{
  return word | ((uint32_t)flag << (uint32_t)shift);
}

ra8_err_t ra8_tz_ipc_attribution_encode(const ra8_tz_ipc_attribution_t* cfg,
                                        uint32_t*                       out_ipcsar,
                                        uint32_t*                       out_ipcpar)
{
  if ((cfg == nullptr) || (out_ipcsar == nullptr) || (out_ipcpar == nullptr)) {
    return k_ra8_err_null_ptr;
  }

  uint32_t ipcsar = 0U;
  uint32_t ipcpar = 0U;

  for (uint8_t index = 0U; index < (uint8_t)k_ra8_tz_ipc_target_count; index++) {
    const ra8_tz_ipc_target_attr_t attr  = cfg->target[index];
    const uint8_t                  world = (uint8_t)attr.world;
    const uint8_t                  privs = (uint8_t)attr.access;
    /* Validation: an out-of-range enum would otherwise shift a value wider
     * than one bit into the word and corrupt its neighbours. */
    if ((world > (uint8_t)k_ra8_tz_ipc_world_non_secure) ||
        (privs > (uint8_t)k_ra8_tz_ipc_access_unprivileged)) {
      return k_ra8_err_invalid_arg;
    }
    ipcsar = internal_apply(ipcsar, s_target_shift[index], world);
    ipcpar = internal_apply(ipcpar, s_target_shift[index], privs);
  }

  /* Post: both outputs written, or neither -- every early return above
   * happens before the first store. */
  *out_ipcsar = ipcsar;
  *out_ipcpar = ipcpar;
  return k_ra8_ok;
}

ra8_err_t ra8_tz_ipc_attribution_cpu1_pingpong(ra8_tz_ipc_attribution_t* out_cfg)
{
  if (out_cfg == nullptr) {
    return k_ra8_err_null_ptr;
  }

  for (uint8_t index = 0U; index < (uint8_t)k_ra8_tz_ipc_target_count; index++) {
    out_cfg->target[index].world  = k_ra8_tz_ipc_world_secure;
    out_cfg->target[index].access = k_ra8_tz_ipc_access_privileged;
  }

  /* CPU1 is always Non-Secure, so it reaches CPU0 over IPC0 channel 0 and is
   * reached over IPC1 channel 0. Nothing else crosses the boundary, and
   * nothing at all is handed to Unprivileged code. */
  out_cfg->target[k_ra8_tz_ipc_target_channel0].world = k_ra8_tz_ipc_world_non_secure;
  out_cfg->target[k_ra8_tz_ipc_target_channel2].world = k_ra8_tz_ipc_world_non_secure;
  return k_ra8_ok;
}

ra8_err_t ra8_tz_secure_boot_security_init_map(const ra8_tz_ipc_attribution_t* cfg)
{
  uint32_t        ipcsar = 0U;
  uint32_t        ipcpar = 0U;
  const ra8_err_t err    = ra8_tz_ipc_attribution_encode(cfg, &ipcsar, &ipcpar);
  if (err != k_ra8_ok) {
    /* Refused before PRCR_S is opened: a bad descriptor leaves the
     * write-protect gate closed and CPSCU exactly as it was. */
    return err;
  }
  return ra8_tz_secure_boot_security_init(ipcsar, ipcpar);
}
