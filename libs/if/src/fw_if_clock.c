/**
 * @file fw_if_clock.c
 * @brief The `fw_clock` facade: validation, dispatch, and the one policy
 * decision the port owns.
 * @ingroup grp_fw_clock
 *
 * @par Tag
 * [Ring 2 / Interface] {World: Any}
 *
 * @details
 * Everything chip-specific lives behind ::fw_clock_iface_t, so what is left
 * here is small on purpose: reject a malformed request, refuse an unbound
 * handle, call the binding, and refuse a binding answer that cannot be true.
 *
 * That last one is the reason this file exists rather than the header being
 * the whole port. A clock rate of zero is not a rate; a binding that returns
 * ::k_ra8_ok with `*out_hz == 0` has a bug, and a driver that divides by it
 * gets undefined behaviour several frames later in code that is not the buggy
 * code. Catching it at the seam turns that into ::k_ra8_err_invalid_state at
 * the call that caused it. `fw_if_fs.c` rejects backend contract violations
 * the same way and for the same reason.
 *
 * ::fw_clock_require is the port's only policy, and it is deliberately a
 * comparison rather than a search: see the note on its declaration for why
 * raising a shared bus clock is not a portable driver's decision to make.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fw_if_clock.h"
#include "ra8_err.h"

/**
 * @brief Whether a module names a kind this contract enumerates.
 *
 * @details
 * `k_fw_clock_module_none` is rejected along with anything past the last
 * enumerator: a zeroed ::fw_clock_module_t is the shape a caller gets by
 * forgetting to fill one in, and answering it would hide that mistake. The
 * index is not range-checked here -- how many instances exist is a board fact,
 * so ::fw_clock_iface_t::has_module owns it.
 */
static bool internal_kind_valid(fw_clock_module_t module) {
  return (module.kind != k_fw_clock_module_none) &&
         ((uint8_t)module.kind < (uint8_t)K_FW_CLOCK_MODULE_KIND_COUNT);
}

/** @brief Common entry guard: bound handle, enumerated kind. */
static ra8_err_t internal_check(const fw_clock_t *clk, fw_clock_module_t module) {
  if (clk == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if (!clk->bound) {
    return k_ra8_err_not_initialized;
  }
  if (!internal_kind_valid(module)) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

ra8_err_t fw_clock_bind(fw_clock_t *clk, const fw_clock_iface_t *iface, void *ctx) {
  if ((clk == nullptr) || (iface == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  /* A NULL op is a malformed binding, not a declined capability: a binding
   * that cannot gate a module says so from set_gate. */
  if ((iface->rate_for == nullptr) || (iface->set_gate == nullptr) || (iface->has_module == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  clk->iface = iface;
  clk->ctx   = ctx;
  clk->bound = true;
  return k_ra8_ok;
}

ra8_err_t fw_clock_rate_for(const fw_clock_t *clk, fw_clock_module_t module, uint32_t *out_hz) {
  if (out_hz == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  *out_hz = 0U;

  const ra8_err_t guard = internal_check(clk, module);
  if (guard != k_ra8_ok) {
    return guard;
  }

  uint32_t        hz  = 0U;
  const ra8_err_t err = clk->iface->rate_for(clk->ctx, module, &hz);
  if (err != k_ra8_ok) {
    return err;
  }
  if (hz == 0U) {
    /* Zero is not a rate. Refusing here names the buggy binding instead of
     * leaving a division by zero in whichever driver asked. */
    return k_ra8_err_invalid_state;
  }
  *out_hz = hz;
  return k_ra8_ok;
}

ra8_err_t fw_clock_require(const fw_clock_t *clk, fw_clock_module_t module, uint32_t min_hz,
                           uint32_t *out_hz) {
  if (min_hz == 0U) {
    /* A floor of zero asks nothing; the caller wanted fw_clock_rate_for. */
    if (out_hz != nullptr) {
      *out_hz = 0U;
    }
    return k_ra8_err_invalid_arg;
  }

  const ra8_err_t err = fw_clock_rate_for(clk, module, out_hz);
  if (err != k_ra8_ok) {
    return err;
  }
  if (*out_hz < min_hz) {
    /* The rate stays written: a driver reports what it found rather than
     * guessing, and the board decides whether to reprogram the tree. */
    return k_ra8_err_not_supported;
  }
  return k_ra8_ok;
}

ra8_err_t fw_clock_enable(const fw_clock_t *clk, fw_clock_module_t module) {
  const ra8_err_t guard = internal_check(clk, module);
  if (guard != k_ra8_ok) {
    return guard;
  }
  return clk->iface->set_gate(clk->ctx, module, true);
}

ra8_err_t fw_clock_disable(const fw_clock_t *clk, fw_clock_module_t module) {
  const ra8_err_t guard = internal_check(clk, module);
  if (guard != k_ra8_ok) {
    return guard;
  }
  return clk->iface->set_gate(clk->ctx, module, false);
}

ra8_err_t fw_clock_has_module(const fw_clock_t *clk, fw_clock_module_t module,
                              bool *out_present) {
  if (out_present == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  *out_present = false;

  const ra8_err_t guard = internal_check(clk, module);
  if (guard != k_ra8_ok) {
    return guard;
  }

  bool            present = false;
  const ra8_err_t err     = clk->iface->has_module(clk->ctx, module, &present);
  if (err != k_ra8_ok) {
    return err;
  }
  *out_present = present;
  return k_ra8_ok;
}
