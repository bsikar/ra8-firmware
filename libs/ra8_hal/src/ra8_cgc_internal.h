/**
 * @file ra8_cgc_internal.h
 * @brief Cross-TU sharing for the split Clock Generation Circuit driver.
 * @ingroup grp_hal_system
 *
 * @details
 * The CGC driver is split across several translation units that all live
 * in `libs/ra8_hal/src/`:
 *
 *  - `ra8_cgc.c`        -- core clock-tree bring-up (PLL1, SCKDIVCR, MRMS
 *                         wait states, SCICLK) plus runtime reconfigure and
 *                         oscillation-stop detection.
 *  - `ra8_cgc_usb.c`    -- PLL2 bring-up and the USB-FS / USB-HS clock paths.
 *  - `ra8_cgc_eswclk.c` -- the Ethernet-switch (ESWCLK / ESWPHYCLK) path.
 *
 * Any symbol referenced by more than one of those units is declared here so
 * there is exactly one definition and no implicit cross-TU reference. The
 * bounded OSCSF poll helpers and the HOCO-restart helper live in this seam
 * because the USB and ESW paths reuse the same hardware handshakes the core
 * bring-up established. The shared `k_ra8_cgc_quarters_per_unit` PLL-multiplier
 * scale factor is the only member of ::ra8_cgc_byte_const_t referenced from
 * more than one unit, so the whole enum block lives here.
 *
 * This header is internal to the CGC driver -- it is NOT a public API and
 * must not be included outside `libs/ra8_hal/src/ra8_cgc*.c`.
 *
 * @par Tag ring/world:
 * Ring 1 (HAL), World S (secure-callable). Same ring/world as the parent
 * `ra8_cgc.c`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"

/**
 * @enum ra8_cgc_byte_const_t
 * @brief Bit-width / Hz-conversion constants used across the CGC driver.
 *
 * @details
 * ::k_ra8_cgc_quarters_per_unit is referenced by both the core PLL1 path
 * (`ra8_cgc.c`) and the PLL2 path (`ra8_cgc_usb.c`), so the whole block lives
 * in this shared seam. The two Hz-conversion members are used only by the
 * MRMS wait-state stage in `ra8_cgc.c`, but they ride along so the multiplier
 * scale factor keeps its documented neighbours.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_cgc_quarters_per_unit = 4UL,       /**< PLL multiplier 0.25 step.      */
  k_ra8_cgc_hz_per_mhz        = 1000000UL, /**< Hz per MHz.                    */
  k_ra8_cgc_mr_min_hz         = 1UL,       /**< Below this, MR*FREQ field = 0. */
} ra8_cgc_byte_const_t;

/**
 * @brief Bounded poll on a single OSCSF flag becoming set.
 *
 * @details
 * Shared between the core PLL1 bring-up (`ra8_cgc.c`) and the PLL2 / USB
 * paths (`ra8_cgc_usb.c`). Spins up to ::k_ra8_cgc_osc_spin_limit iterations.
 *
 * @param[in] bit OSCSF bit number to wait on (e.g. ::k_ra8_oscsf_bit_pll1sf).
 *
 * @return ra8_err_t error code.
 * @retval k_ra8_ok            Flag observed set.
 * @retval k_ra8_err_hw_timeout Spin limit exhausted.
 *
 * @pre  The associated oscillator / PLL has been started.
 * @pre  Caller is single-threaded init context.
 *
 * @post On k_ra8_ok return, the polled bit reads as 1.
 * @post Iteration count is bounded.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t ra8_cgc_wait_oscsf_set(uint8_t bit);

/**
 * @brief Bounded poll on a single OSCSF flag becoming clear.
 *
 * @details
 * Shared between the core PLL1 bring-up (`ra8_cgc.c`) and the PLL2 path
 * (`ra8_cgc_usb.c`). Spins up to ::k_ra8_cgc_pll_spin_limit iterations.
 *
 * @param[in] bit OSCSF bit number to wait on.
 *
 * @return ra8_err_t error code.
 * @retval k_ra8_ok            Flag observed clear.
 * @retval k_ra8_err_hw_timeout Spin limit exhausted.
 *
 * @pre  The associated oscillator / PLL has just been stopped.
 * @pre  Caller is single-threaded init context.
 *
 * @post On k_ra8_ok return, the polled bit reads as 0.
 * @post Iteration count is bounded.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t ra8_cgc_wait_oscsf_clear(uint8_t bit);

/**
 * @brief Ensure HOCO is running so a USBCKCR/USB60CKCR/ESWCKCR SREQ->SRDY
 *        handshake can drain.
 *
 * @details
 * USBCKCR / USB60CKCR / ESWCKCR all share HOCO as their reset-default
 * source. The SREQ -> SRDY synchronizer chain crosses from ICLK into the
 * CURRENT clock source's domain (HOCO at boot). If HOCO is stopped
 * (HCSTP = 1) the handshake never completes. ::ra8_cgc_init leaves HOCO
 * stopped because it switches SCKSCR to PLL1, so this helper restarts HOCO
 * under a PRCR-CGC unlock window (HOCOCR IS PRCR-protected; writes outside
 * the window are silently dropped) and then waits OSCSF.HOCOSF outside the
 * PRCR window (read-only). Defined in `ra8_cgc_usb.c` and reused by
 * `ra8_cgc_eswclk.c`.
 *
 * @return ::ra8_err_t error code.
 * @retval k_ra8_ok HOCO running, OSCSF.HOCOSF asserted.
 * @retval k_ra8_err_hw_timeout HOCOSF never asserted.
 *
 * @pre Caller is single-threaded init context.
 * @pre PRCR is currently locked (helper takes its own window).
 *
 * @post On k_ra8_ok HOCO is running.
 * @post PRCR is re-locked.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t ra8_cgc_ensure_hoco_running_for_usb_ck(void);
