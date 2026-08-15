/**
 * @file ra8_nsc_periph_init.c
 * @brief NSC veneer: secure peripheral substrate bring-up
 *
 * @par Tag
 * [Ring 4 / NSC] {World: NSC}
 *
 * @details
 * scaffold. Runs the substrate init dance that the NS
 * world cannot do because the MSTP / CGC / ICU register windows
 * live in the secure region partitioning.
 *
 * The function is idempotent: callers can fire it more than once
 * and get ``k_ra8_ok`` for every call after the first.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_dma.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_log.h"
#include "ra8_mstp.h"
#include "ra8_nsc.h"
#include "ra8_nsc_veneer.h"
#include "ra8_pwr.h"

#ifdef RA8_APP_UART_LOG
/* The e-reader (src/app) defines RA8_APP_UART_LOG to mirror ra8_log onto the SCI8
 * J-Link console, so the logs appear on the UART (ra8_emulator's on-screen console
 * + a real serial terminal) and not only on the ITM/SWO trace. Gated so other
 * ra8_nsc consumers do not pick up a ra8_board dependency. */
#include "ra8_board_ek_ra8d2.h"
#endif

static const char* s_tag = "NSCPRH";

static bool s_initialized = false;

#ifdef RA8_APP_UART_LOG
/**
 * @enum ra8_nsc_uart_log_cfg_t
 * @brief SCI8 console settings for the optional ra8_log->UART mirror.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_nsc_uart_log_baud = 115200U, /**< SCI8 J-Link console baud (8N1). */
} ra8_nsc_uart_log_cfg_t;

/**
 * @brief ra8_log byte sink that forwards one log byte to the SCI8 console.
 *
 * @details Installed via ::ra8_log_set_byte_sink, so every byte ra8_log would have
 *          written to the ITM stimulus port is instead transmitted on the SCI8
 *          console. That makes log output visible on the UART -- ra8_emulator's
 *          on-screen console panel and a real serial terminal -- not only on the
 *          SWO/ITM trace.
 *
 * @param[in] ctx  Unused opaque cookie (always NULL for this sink).
 * @param[in] byte The single log byte to transmit.
 *
 * @pre ::ra8_board_uart_console_init has succeeded.
 * @pre The SCI8 console is the intended log destination for this build.
 * @post The byte has been handed to the SCI8 console writer.
 * @post No other module state is modified.
 *
 * @note Not thread-safe; the console writer is single-producer.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_uart_log_sink(void* ctx, uint8_t byte)
{
  (void)ctx;
  (void)ra8_board_uart_console_write(&byte, 1U);
}
#endif

/**
 * @brief NSC veneer: bring up the secure-side peripheral substrate.
 *
 * @details
 * Sequences the four substrate init calls that the Non-Secure world
 * cannot perform because the MSTP / CGC / ICU / DMA register windows
 * live in the secure region partitioning:
 * ``ra8_mstp_init`` -> ``ra8_pwr_init`` -> ``ra8_isr_init`` -> ``ra8_dma_init``.
 *
 * The function is idempotent: subsequent calls return ``k_ra8_ok``
 * without redoing the work. On failure the latched ``s_initialized``
 * flag stays false so a future call may retry from scratch.
 *
 * @return ra8_err_t outcome.
 * @retval k_ra8_ok                  Substrate up (or already up).
 * @retval k_ra8_err_hw_init_failed  One of the secure init steps failed.
 *
 * @pre Reset vector has handed over to the firmware.
 * @pre TrustZone SAU has been programmed (single-world or NS region carved).
 * @post On success the secure peripheral substrate is fully up.
 * @post On failure the substrate is in an indeterminate partial state;
 *       the caller may retry.
 *
 * @par TrustZone:
 *   NS->S boundary via ``cmse_nonsecure_entry``. Argument-less, scalar
 *   return -- nothing crosses the boundary that needs range-checking.
 *
 * @note Thread-safe: no -- intended to be called once at boot.
 * @since 0.1.0
 */
RA8_NSC_VENEER ra8_err_t ra8_nsc_periph_init(void)
{
  if (s_initialized) {
    return k_ra8_ok;
  }

  ra8_err_t err = ra8_mstp_init();
  if (err != k_ra8_ok) { /* GCOVR_EXCL_BR_LINE */
    /* GCOVR_EXCL_START */
    ra8_log_error_val(s_tag, "ra8_mstp_init", (uint32_t)err);
    return k_ra8_err_hw_init_failed;
    /* GCOVR_EXCL_STOP */
  }

  err = ra8_pwr_init();
  if (err != k_ra8_ok) { /* GCOVR_EXCL_BR_LINE */
    /* GCOVR_EXCL_START */
    ra8_log_error_val(s_tag, "ra8_pwr_init", (uint32_t)err);
    return k_ra8_err_hw_init_failed;
    /* GCOVR_EXCL_STOP */
  }

  err = ra8_isr_init();
  if (err != k_ra8_ok) { /* GCOVR_EXCL_BR_LINE */
    /* GCOVR_EXCL_START */
    ra8_log_error_val(s_tag, "ra8_isr_init", (uint32_t)err);
    return k_ra8_err_hw_init_failed;
    /* GCOVR_EXCL_STOP */
  }

  err = ra8_dma_init();
  if (err != k_ra8_ok) { /* GCOVR_EXCL_BR_LINE */
    /* GCOVR_EXCL_START */
    ra8_log_error_val(s_tag, "ra8_dma_init", (uint32_t)err);
    return k_ra8_err_hw_init_failed;
    /* GCOVR_EXCL_STOP */
  }

#ifdef RA8_APP_UART_LOG
  /* Bring up the SCI8 console and redirect ra8_log to it so the logs land on the
   * UART (ra8_emulator console + serial), not only ITM. ra8_cgc_init already ran in
   * SystemInit. Best-effort: a console bring-up failure must not fail the
   * substrate, so its result is dropped and ra8_log keeps its ITM default. */
  if (ra8_board_uart_console_init((uint32_t)k_ra8_nsc_uart_log_baud) == k_ra8_ok) {
    ra8_log_set_byte_sink(internal_uart_log_sink, nullptr);
  }
#endif

  s_initialized = true;
  ra8_log_info(s_tag, "secure substrate up");
  return k_ra8_ok;
}
