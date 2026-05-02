/**
 * @file ra_eth_coma.c
 * @brief Ethernet Common Agent driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * driver for the RA8D2 COMA block. Shares the ESWM MSTP
 * gate with the rest of the ethernet subsystem. Every register
 * access carries a HUM Ch 31 citation.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_eth_coma.h"

#include <stdint.h>

#include "ra8d2_ether_regs.h"
#include "ra8d2_mstp_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_mstp.h"

static const char* s_tag = "ETHCMA";

static ra_eth_coma_event_fn_t s_coma_fn;
static void*                  s_coma_ctx;

/**
 * @brief Implementation of ra_eth_coma_init (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_err_t ra_eth_coma_init(void)
{
  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C" p 446 */
  const ra_err_t mst_err = ra_mstp_enable(k_ra_mstp_eswm);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "coma_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  volatile r_coma_regs_t* reg = ra_coma();
  /* HUM Ch 31 "Ethernet Common Agent (COMA)" p 1590 */
  reg->COMA_CTRL = 0U;
  reg->COMA_STS  = 0U;
  reg->COMA_IE   = 0U;
  reg->COMA_ICLR = 0U;
  ra_log_info(s_tag, "coma_init");
  return k_ra_ok;
}

/**
 * @brief Implementation of ra_eth_coma_deinit (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_err_t ra_eth_coma_deinit(void)
{
  volatile r_coma_regs_t* reg = ra_coma();
  /* HUM Ch 31 "Ethernet Common Agent (COMA)" p 1590 */
  reg->COMA_CTRL = 0U;
  reg->COMA_IE   = 0U;
  s_coma_fn      = nullptr;
  s_coma_ctx     = nullptr;
  return ra_mstp_disable(k_ra_mstp_eswm);
}

/**
 * @brief Implementation of ra_eth_coma_get_status (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] out_mask See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_err_t ra_eth_coma_get_status(uint32_t* out_mask)
{
  RA_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  /* HUM Ch 31 "Ethernet Common Agent (COMA)" p 1590 */
  *out_mask = ra_coma()->COMA_STS;
  return k_ra_ok;
}

/**
 * @brief Implementation of ra_eth_coma_clear_status (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] mask See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_err_t ra_eth_coma_clear_status(uint32_t mask)
{
  volatile r_coma_regs_t* reg = ra_coma();
  /* HUM Ch 31 "Ethernet Common Agent (COMA)" p 1590 */
  reg->COMA_ICLR = mask;
  reg->COMA_STS  = reg->COMA_STS & ~mask;
  return k_ra_ok;
}

/**
 * @brief Implementation of ra_eth_coma_attach_handler (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] fn See implementation.
 * @param[in] ctx See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_err_t ra_eth_coma_attach_handler(ra_eth_coma_event_fn_t fn, void* ctx)
{
  s_coma_fn  = fn;
  s_coma_ctx = ctx;
  return k_ra_ok;
}

/**
 * @brief Implementation of ra_eth_coma_dispatch (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
void ra_eth_coma_dispatch(void)
{
  volatile r_coma_regs_t* reg = ra_coma();
  /* HUM Ch 31 "Ethernet Common Agent (COMA)" p 1590 */
  const uint32_t               mask = reg->COMA_STS;
  const ra_eth_coma_event_fn_t fn   = s_coma_fn;
  void* const                  ctx  = s_coma_ctx;
  reg->COMA_ICLR                    = mask;
  reg->COMA_STS                     = 0U;
  if (fn != nullptr) {
    fn(ctx, mask);
  }
}

/**
 * @brief Implementation of ra_eth_coma_enter_stop (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_err_t ra_eth_coma_enter_stop(void)
{
  /* HUM Ch 31 "Ethernet Common Agent (COMA)" p 1590 */
  ra_coma()->COMA_CTRL = 0U;
  return ra_mstp_disable(k_ra_mstp_eswm);
}

/**
 * @brief Implementation of ra_eth_coma_exit_stop (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_err_t ra_eth_coma_exit_stop(void)
{
  return ra_mstp_enable(k_ra_mstp_eswm);
}
