/**
 * @file ra_trng.c
 * @brief True Random Number Generator (TRNG) driver implementation
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_trng.h"

#include <stdint.h>

#include "ra8d2_trng_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_mstp.h"

static const char* s_tag = "TRNG";

/**
 * @enum ra_trng_lcg_t
 * @brief Linear-congruential generator constants used by the simulator
 *        fallback inside `ra_trng_read`.
 */
typedef enum : uint32_t {
  k_ra_trng_lcg_mul  = 1103515245UL, /**< LCG multiplier (glibc rand). */
  k_ra_trng_lcg_add  = 12345UL,      /**< LCG additive constant.       */
  k_ra_trng_lcg_seed = 0xC0FFEEU,    /**< Initial state seed.          */
} ra_trng_lcg_t;

/**
 * @brief Counter used by the simulator pseudo-random fallback.
 *
 * @details
 * Static file-scope state so each call to `ra_trng_read()` advances
 * the LCG and returns a new 32-bit word. Not referenced in target
 * builds.
 */
static uint32_t s_ra_trng_sim_state = (uint32_t)k_ra_trng_lcg_seed;

[[nodiscard]] ra_err_t ra_trng_init(void)
{
  /* TRNG and SCE share the RSIP block (MSTPC31). The ref count
   * lets ra_sce_init and ra_trng_init coexist without one
   * disabling the other when it tears down.
   * HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C", p 447 */
  const ra_err_t mst_err = ra_mstp_enable(k_ra_mstp_rsip);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "trng_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  volatile r_trng_regs_t* reg = ra_trng();
  reg->TRNGCR                 = 0U;
  reg->TRNGSR                 = 0U;
  reg->TRNGDO                 = 0U;
  reg->TRNGSEED               = 0U;
  reg->TRNGCR                 = (uint8_t)k_ra_trng_mask_en;
  s_ra_trng_sim_state         = (uint32_t)k_ra_trng_lcg_seed;
  ra_log_info(s_tag, "trng_init");
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_trng_read(uint32_t* out)
{
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");

  volatile r_trng_regs_t* reg = ra_trng();
#ifdef RA_SIMULATOR_MODE
  /* Host simulator: the hardware is not present, advance a tiny
   * deterministic LCG so tests observe a unique word per call.
   */
  s_ra_trng_sim_state =
    (s_ra_trng_sim_state * (uint32_t)k_ra_trng_lcg_mul) + (uint32_t)k_ra_trng_lcg_add;
  reg->TRNGDO = s_ra_trng_sim_state;
  reg->TRNGSR = (uint8_t)(reg->TRNGSR | (uint8_t)k_ra_trng_mask_drdy);
#endif
  if ((reg->TRNGSR & (uint8_t)k_ra_trng_mask_drdy) == 0U) {
    return k_ra_err_hw_not_ready;
  }
  *out = reg->TRNGDO;
  /* Clear DRDY so the next call waits for a fresh sample. */
  reg->TRNGSR = (uint8_t)(reg->TRNGSR & (uint8_t)~(uint8_t)k_ra_trng_mask_drdy);
  return k_ra_ok;
}
