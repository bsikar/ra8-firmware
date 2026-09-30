/**
 * @file fw_if_clock_ra8.c
 * @brief The three `fw_if_clock` ops, over `ra8_cgc` and `ra8_mstp`.
 * @ingroup grp_fw_clock
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Every op resolves through ::fw_clock_ra8_resolve first, so the table proven
 * by the host test is the table these use. The port facade has already
 * rejected a NULL output, an unbound handle and an out-of-range kind before
 * control reaches here, so these functions check only what the chip layer can
 * still refuse.
 *
 * @par Gating is reference-counted, and that is deliberate
 * `ra8_mstp_enable` and `ra8_mstp_disable` keep a per-bit reference count:
 * enabling an already-running module succeeds without touching hardware, and
 * disabling one another user still holds leaves it running. Passing that
 * through unchanged is the right behaviour for a port whose whole purpose is
 * that two portable drivers can each ask for their own block without knowing
 * about each other. The one sharp edge is inherited: disabling a module whose
 * count is already zero returns ::k_ra8_err_invalid_state, because an
 * unbalanced release is a bug in the caller and not something to swallow.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "fw_if_clock_ra8.h"

#include <stdbool.h>
#include <stdint.h>

#include "fw_if_clock.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_mstp.h"

/**
 * @brief Read the rate of the domain feeding one module.
 *
 * @param[in]  ctx    Unused; the RA8 clock tree is a chip singleton.
 * @param[in]  module Neutral module, @p index being the chip instance.
 * @param[out] out_hz Rate in Hz on success.
 * @return ::k_ra8_ok, ::k_ra8_err_not_found when no row exists,
 *         ::k_ra8_err_not_supported when the row carries no feed domain, or
 *         whatever `ra8_cgc_get_clock_hz` reported.
 */
static ra8_err_t internal_rate_for(void *ctx, fw_clock_module_t module, uint32_t *out_hz)
{
  (void)ctx;

  fw_clock_ra8_row_t row = {0};
  const ra8_err_t    err = fw_clock_ra8_resolve(module, &row);
  if (err != k_ra8_ok) {
    return err;
  }
  if (!row.has_domain) {
    return k_ra8_err_not_supported;
  }

  return ra8_cgc_get_clock_hz(row.domain, out_hz);
}

/**
 * @brief Ungate or gate one module's clock.
 *
 * @param[in] ctx    Unused.
 * @param[in] module Neutral module, @p index being the chip instance.
 * @param[in] on     True to ungate, false to release.
 * @return ::k_ra8_ok, ::k_ra8_err_not_found when no row exists,
 *         ::k_ra8_err_not_supported when the row has no module-stop bit, or
 *         whatever `ra8_mstp_enable` / `ra8_mstp_disable` reported.
 */
static ra8_err_t internal_set_gate(void *ctx, fw_clock_module_t module, bool on)
{
  (void)ctx;

  fw_clock_ra8_row_t row = {0};
  const ra8_err_t    err = fw_clock_ra8_resolve(module, &row);
  if (err != k_ra8_ok) {
    return err;
  }
  if (!row.has_gate) {
    return k_ra8_err_not_supported;
  }

  return on ? ra8_mstp_enable(row.gate) : ra8_mstp_disable(row.gate);
}

/**
 * @brief Whether this adapter can resolve a module instance.
 *
 * @details
 * Reports resolvability, not silicon: see the header on what a `false` here
 * does and does not claim.
 *
 * @param[in]  ctx         Unused.
 * @param[in]  module      Neutral module.
 * @param[out] out_present True when a row exists.
 * @return ::k_ra8_ok, or ::k_ra8_err_invalid_arg for a kind outside the
 *         enumeration.
 */
static ra8_err_t internal_has_module(void *ctx, fw_clock_module_t module, bool *out_present)
{
  (void)ctx;

  if (out_present == nullptr) {
    return k_ra8_err_invalid_arg;
  }

  fw_clock_ra8_row_t row = {0};
  const ra8_err_t    err = fw_clock_ra8_resolve(module, &row);
  if (err == k_ra8_err_invalid_arg) {
    *out_present = false;
    return k_ra8_err_invalid_arg;
  }

  *out_present = (err == k_ra8_ok);
  return k_ra8_ok;
}

static const fw_clock_iface_t k_internal_iface = {
    .rate_for   = internal_rate_for,
    .set_gate   = internal_set_gate,
    .has_module = internal_has_module,
};

const fw_clock_iface_t *fw_clock_ra8_iface(void)
{
  return &k_internal_iface;
}

ra8_err_t fw_clock_ra8_bind(fw_clock_t *clk)
{
  return fw_clock_bind(clk, &k_internal_iface, nullptr);
}
