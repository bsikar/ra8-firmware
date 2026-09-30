/**
 * @file fw_if_clock_ra8.h
 * @brief RA8 chip adapter for the neutral `fw_if_clock` port.
 * @ingroup grp_fw_clock
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * `fw_if_clock.h` says a request names a neutral module kind and an instance,
 * and that "a binding supplied by the chip and the board resolves it against a
 * clock profile they own between them". This file is the chip half of that
 * sentence for the RA8D2. It answers two questions and refuses the rest:
 *
 *   - which clock-tree domain feeds a given block, so ::fw_clock_rate_for can
 *     read it back through `ra8_cgc_get_clock_hz`;
 *   - which module-stop bit gates it, so ::fw_clock_enable and
 *     ::fw_clock_disable can reach `ra8_mstp_enable` / `ra8_mstp_disable`.
 *
 * @par What "index" means here
 * The index in this adapter is the *chip* instance: UART 3 is SCI3, I2C 1 is
 * IIC1. That is deliberately **not** the dense board numbering the port
 * describes. Renumbering is the board's job, and the board layer that owns the
 * pin map is the only thing that knows a given EK wires SCI3 and SCI9 and
 * nothing else. A board binding that wants dense numbering wraps this adapter
 * and translates on the way in; this file stays a statement about silicon.
 *
 * @par The table is grounded, not exhaustive
 * Every row below cites where its pairing came from -- a first-party driver or
 * board file that already reads that domain for that block, or the module-stop
 * table in `ra8_mstp_regs.h`. Rows that could not be grounded are absent
 * rather than guessed, because a wrong feed domain here becomes a wrong baud
 * divisor or a wrong sampling period in a driver that trusted it. An absent or
 * partial row answers honestly:
 *
 *   - no row at all -> ::k_ra8_err_not_found, and ::fw_clock_has_module
 *     reports `false`;
 *   - a row whose feed domain is not established in-tree -> gating works,
 *     ::fw_clock_rate_for returns ::k_ra8_err_not_supported;
 *   - a row with no module-stop bit (the core, the flash interface) ->
 *     the rate reads, ::fw_clock_enable returns ::k_ra8_err_not_supported.
 *
 * So `false` from ::fw_clock_has_module means "this binding cannot resolve
 * that instance", not "the silicon lacks it". Filling a gap is a deliberate
 * act with the hardware manual open, which is the point.
 *
 * @par World
 * `ra8_cgc.h` and `ra8_mstp.h` are both `{World: S}`, so this adapter is too.
 * A Non-Secure image needs a sibling bound to the `ra8_nsc_cgc` veneer; that
 * veneer forwards the rate query but publishes no module-stop entry point
 * today, so the gating half has no NS route yet and this file does not pretend
 * otherwise.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#include "fw_if_clock.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_mstp.h"

/**
 * @struct fw_clock_ra8_row_t
 * @brief What this adapter knows about one module instance.
 *
 * @details
 * Both `has_` flags can be false at once for a kind the adapter carries no row
 * for; ::fw_clock_ra8_resolve reports that as ::k_ra8_err_not_found rather
 * than handing back a row of zeroes.
 */
typedef struct fw_clock_ra8_row_s {
  ra8_clock_id_t domain;
  ra8_mstp_t     gate;
  bool           has_domain;
  bool           has_gate;
} fw_clock_ra8_row_t;

/**
 * @brief Resolve a neutral module onto its RA8 domain and module-stop bit.
 *
 * @details
 * Pure table lookup: no register is touched, which is what lets the mapping be
 * tested on the host without a fake peripheral block. The three ops in this
 * adapter all funnel through it, so a row proven here is the row they use.
 *
 * @param[in]  module  Neutral module, with @p index as the chip instance.
 * @param[out] out_row On success, what is known about that instance.
 * @return ::k_ra8_ok, ::k_ra8_err_invalid_arg for a NULL output or a kind
 *         outside ::fw_clock_module_kind_t, or ::k_ra8_err_not_found when this
 *         adapter carries no row for that kind and index.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
fw_clock_ra8_resolve(fw_clock_module_t module, fw_clock_ra8_row_t *out_row);

/**
 * @brief The ops struct this adapter fills.
 *
 * @details
 * Static storage with no context, because the RA8 clock tree and module-stop
 * block are chip singletons: ::fw_clock_bind is called with a NULL `ctx` and
 * the ops ignore it. The pointer outlives any handle bound to it.
 *
 * @return Borrowed ops struct, never NULL.
 * @since 0.1.0
 */
const fw_clock_iface_t *fw_clock_ra8_iface(void);

/**
 * @brief Bind a handle to this chip adapter.
 *
 * @details
 * Convenience over `fw_clock_bind(clk, fw_clock_ra8_iface(), nullptr)`, so a
 * composition root does not have to name the context it is not passing.
 *
 * @param[out] clk Caller-owned handle, contents ignored on entry.
 * @return As ::fw_clock_bind.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t fw_clock_ra8_bind(fw_clock_t *clk);

#ifdef __cplusplus
}
#endif
