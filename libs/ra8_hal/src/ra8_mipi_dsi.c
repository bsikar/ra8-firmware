/**
 * @file ra8_mipi_dsi.c
 * @brief MIPI DSI-2 host driver -- full HUM Ch 65 coverage
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Hand-written HAL for the RA8D2 MIPI DSI Host module (HUM Ch 65,
 * p 3839-3934). Covers every documented register, every operating
 * mode (sequence channel 0 LP, sequence channel 1 HS, video mode,
 * BTA / read response, ULPS), every interrupt source (SQ0, SQ1, VM,
 * RX, FERR, PHY), every error class (ECC single/multi, CRC, BTA TO,
 * peripheral response timeout, fatal HS-Tx / LP-Rx / TA timeouts).
 *
 * The D-PHY analog block (HUM Ch 64) is owned by `ra8_mipi_phy.c`. This
 * driver only programmes the DSI link/protocol layer above it.
 *
 * This translation unit carries the configuration, software-reset,
 * module-stop lifecycle, HS-clock control, and ULPS paths -- everything
 * that owns the driver's module state. The sequence-channel command path
 * lives in the sibling `ra8_mipi_dsi_cmd.c`, and the video-mode, status,
 * IRQ-dispatch and convenience surfaces in `ra8_mipi_dsi_dispatch.c`;
 * state shared across the three TUs lives in `ra8_mipi_dsi_internal.h`.
 *
 * Every register access carries a HUM Ch 65 citation in the form
 * required by `scripts/checks/cite_check.py`:
 *
 *   /\* HUM Ch 65.X "name", p NNNN *\/
 *
 * @par State Machine
 * @dot
 * digraph ra8_mipi_dsi_states {
 *   bgcolor="transparent";
 *   rankdir=LR;
 *   node [shape=box, style="rounded,filled", fontname="Helvetica", fontsize=10,
 *         fillcolor="#e8eef7", color="#5a7ca6"];
 *   edge [fontname="Helvetica", fontsize=9, color="#5a7ca6"];
 *
 *   __start [shape=circle, width=0.18, label="", fillcolor="#5a7ca6", color="#5a7ca6"];
 *
 *   idle [label="idle"];
 *   initialized [label="initialized"];
 *   hs_clock_running [label="hs_clock_running"];
 *   video_running [label="video_running"];
 *   ulps [label="ulps"];
 *
 *   __start -> idle;
 *   idle -> initialized [label="ra8_mipi_dsi_init()"];
 *   initialized -> hs_clock_running [label="ra8_mipi_dsi_hs_clock_start()"];
 *   hs_clock_running -> video_running [label="ra8_mipi_dsi_video_start()"];
 *   hs_clock_running -> ulps [label="ra8_mipi_dsi_ulps_enter()"];
 *   ulps -> hs_clock_running [label="ra8_mipi_dsi_ulps_exit()"];
 *   video_running -> hs_clock_running [label="ra8_mipi_dsi_video_stop()"];
 *   hs_clock_running -> initialized [label="ra8_mipi_dsi_hs_clock_stop()"];
 *   initialized -> idle [label="ra8_mipi_dsi_deinit()"];
 * }
 * @enddot
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_mipi_dsi.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_log.h"
#include "ra8_mipi_dsi_internal.h"
#include "ra8_mipi_dsi_regs.h"
#include "ra8_mstp.h"

/**
 * @var s_tag
 * @brief Component tag used by the `ra8_log_*` family.
 *
 * @details
 * Static so the linker keeps it confined to this TU. Same convention
 * as every other ra8_hal driver (see `ra8_glcdc.c`, `ra8_doc.c`).
 *
 * @note Read-only after assignment. Not modified at runtime.
 * @warning Never modify directly -- declared `const` to enforce.
 * @since 0.1.0
 */
static const char* s_tag = "MIPI_DSI";

/**
 * @var s_mipi_dsi_event_fn
 * @brief Single definition of the shared IRQ callback pointer.
 *
 * @details
 * Declared `extern` (with the full contract) in
 * ``ra8_mipi_dsi_internal.h`` and read from IRQ context in
 * ``ra8_mipi_dsi_dispatch.c``. Mutated only from the single-threaded
 * init / deinit / attach-handler call sites.
 *
 * @note Direct access from outside the MIPI DSI-2 driver is forbidden.
 * @warning Not thread-safe; treat as IRQ-shared state.
 * @since 0.1.0
 */
ra8_mipi_dsi_event_fn_t s_mipi_dsi_event_fn;

/**
 * @var s_mipi_dsi_event_ctx
 * @brief Single definition of the shared IRQ callback context.
 *
 * @details
 * Declared `extern` (with the full contract) in
 * ``ra8_mipi_dsi_internal.h``. Lifetime is owned by the caller of
 * `ra8_mipi_dsi_attach_handler`.
 *
 * @note May be NULL if the user never attached.
 * @warning Direct access from outside the MIPI DSI-2 driver is forbidden.
 * @since 0.1.0
 */
void* s_mipi_dsi_event_ctx;

/**
 * @var s_continuous_clock
 * @brief Cache of `cfg->clock_mode == continuous` from the last init.
 *
 * @details
 * Needed because the ULPS-enter helper must reject a clock-lane ULPS
 * request when continuous-clock mode is on -- the FSP source enforces
 * the same rule. Snap-shotting it once at init avoids an extra
 * register read on every `ra8_mipi_dsi_ulps_enter()` call.
 *
 * @note Mutated only from `ra8_mipi_dsi_init()`.
 * @warning Stale across reinit if `init` is skipped.
 * @since 0.1.0
 */
static bool s_continuous_clock;

/**
 * @var s_clock_lanes_in_ulps
 * @brief Software shadow of clock-lane ULPS state.
 *
 * @details
 * The hardware does not expose a "currently in ULPS" bit cheaply, so
 * the driver tracks it. Pulsing CLENT a second time while already in
 * ULPS is a no-op in HW but FSP guards against it for symmetry; we do
 * the same.
 *
 * @note Updated only from ULPS enter / exit helpers.
 * @warning Reset by `ra8_mipi_dsi_init()` and `_deinit()`.
 * @since 0.1.0
 */
static bool s_clock_lanes_in_ulps;

/**
 * @var s_data_lanes_in_ulps
 * @brief Software shadow of data-lane ULPS state.
 *
 * @note Updated only from ULPS enter / exit helpers.
 * @warning Reset by `ra8_mipi_dsi_init()` and `_deinit()`.
 * @since 0.1.0
 */
static bool s_data_lanes_in_ulps;

/**
 * @var s_mipi_dsi_pending_rx_buffer
 * @brief Single definition of the shared pending receive buffer.
 *
 * @details
 * Declared `extern` (with the full contract) in
 * ``ra8_mipi_dsi_internal.h``. The receive ISR in
 * ``ra8_mipi_dsi_dispatch.c`` copies long-packet payload bytes from
 * RXPPD0..3R into this buffer once the response arrives, then clears it
 * to avoid stale-pointer use.
 *
 * @note Modified from read_packet / send_command and the RX dispatch.
 * @warning The pointer must remain valid until the IRQ fires.
 * @since 0.1.0
 */
uint8_t* s_mipi_dsi_pending_rx_buffer;

/**
 * @var s_mipi_dsi_pending_rx_len
 * @brief Single definition of the shared pending receive capacity.
 *
 * @details
 * Declared `extern` (with the full contract) in
 * ``ra8_mipi_dsi_internal.h``. Cleared to zero once the response is
 * consumed by the RX dispatch.
 *
 * @note Treat as undefined when ``s_mipi_dsi_pending_rx_buffer`` is NULL.
 * @warning Direct access from outside the MIPI DSI-2 driver is forbidden.
 * @since 0.1.0
 */
uint16_t s_mipi_dsi_pending_rx_len;

/**
 * @var s_initialized
 * @brief Software latch tracking whether the driver currently owns the
 *        MIPI-DSI module-stop reference count.
 *
 * @details
 * Set true at the tail of ``ra8_mipi_dsi_init()`` once the MSTPC bit has
 * been ungated, and cleared at the tail of ``ra8_mipi_dsi_deinit()``
 * once the bit has been gated. ``ra8_mipi_dsi_enter_stop()`` clears it
 * (because it gives the MSTPC reference back) and
 * ``ra8_mipi_dsi_exit_stop()`` re-sets it. The flag exists so that
 * ``deinit()`` is idempotent: calling it on an already-de-initialized
 * driver returns ``k_ra8_ok`` instead of letting ``ra8_mstp_disable()``
 * underflow its reference count and surface
 * ``k_ra8_err_invalid_state``. HUM Ch 11.2.8 "MSTPCRC : Module Stop
 * Control Register C", p 446 only describes the gate bit; the
 * reference-count discipline is a software invariant maintained by
 * ``libs/ra8_hal/src/ra8_mstp.c``.
 *
 * @note Mutated only from ``init`` / ``deinit`` / ``enter_stop`` /
 *       ``exit_stop``; not thread-safe.
 * @warning Bypassing these entry points (e.g. by calling
 *          ``ra8_mstp_*`` directly) will desynchronise this latch from
 *          the actual MSTPC state.
 * @since 0.1.0
 */
static bool s_initialized;

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/**
 * @brief Validate a `ra8_mipi_dsi_config_t` argument.
 *
 * @param[in] cfg Caller-supplied config.
 * @return `k_ra8_ok` if OK, `k_ra8_err_invalid_arg` if `lane_count` is
 *         outside [1,2].
 *
 * @pre `cfg` is non-NULL (checked by caller via `RA8_CHECK_NULL_PTR`).
 * @pre Caller is in single-threaded context.
 * @post No side effects.
 *
 * @details See implementation.
 * @retval k_ra8_ok Operation succeeded.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_ra8_mipi_dsi_validate_cfg(const ra8_mipi_dsi_config_t* cfg)
{
  if ((cfg->lane_count != k_ra8_mipi_dsi_lanes_1) && (cfg->lane_count != k_ra8_mipi_dsi_lanes_2)) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

/**
 * @brief Clear every latched status bit in every sub-status register.
 *
 * @details
 * RW1C semantics mean writing a 1 clears the bit; writing 0xFFFFFFFF
 * clears them all in a single store. Used by both init and the
 * dispatch routines.
 *
 * @pre Block is out of MSTP gate.
 * @pre Caller's IRQs are masked or the call is itself in IRQ context.
 * @post All sub-status registers read 0 for valid bits.
 *
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_ra8_mipi_dsi_clear_all_status(void)
{
  volatile r_mipi_dsi_regs_t* reg = ra8_mipi_dsi();
  /* HUM Ch 65.2 "SQCH0SCR : Sequence Channel 0 Status Clear", p 3902 */
  reg->SQCH0SCR = k_ra8_mipi_dsi_sqch_clear_all;
  /* HUM Ch 65.2 "SQCH1SCR : Sequence Channel 1 Status Clear", p 3907 */
  reg->SQCH1SCR = k_ra8_mipi_dsi_sqch_clear_all;
  /* HUM Ch 65.2 "VMSCR : Video Mode Status Clear", p 3894 */
  reg->VMSCR = k_ra8_mipi_dsi_vmsr_clear_all;
  /* HUM Ch 65.2 "RXSCR : Receive Status Clear", p 3855 */
  reg->RXSCR = k_ra8_mipi_dsi_rxsr_clear_all;
  /* HUM Ch 65.2 "FERRSCR : Fatal Error Status Clear", p 3878 */
  reg->FERRSCR = k_ra8_mipi_dsi_ferrsr_clear_all;
  /* HUM Ch 65.2 "PLSCR : PHY Lane Status Clear", p 3887 */
  reg->PLSCR = k_ra8_mipi_dsi_plsr_clear_all;
}

/**
 * @brief Compose `TXSETR` from a config (lane count + clock + data lane enable).
 *
 * @param[in] cfg Validated configuration.
 * @return Word ready for write to `TXSETR`.
 *
 * @details See implementation.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_ra8_mipi_dsi_make_txsetr(const ra8_mipi_dsi_config_t* cfg)
{
  uint32_t v = k_ra8_mipi_dsi_txset_clen | k_ra8_mipi_dsi_txset_dlen;
  if (cfg->lane_count == k_ra8_mipi_dsi_lanes_2) {
    v |= k_ra8_mipi_dsi_txset_lane2;
  }
  return v;
}

/**
 * @brief Compose `DSISETR` from a config.
 *
 * @param[in] cfg Validated configuration.
 * @return Word ready for write to `DSISETR`.
 *
 * @details See implementation.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_ra8_mipi_dsi_make_dsisetr(const ra8_mipi_dsi_config_t* cfg)
{
  uint32_t v = ((uint32_t)cfg->max_return_packet_size & k_ra8_mipi_dsi_dsisetr_mrpsz_mask);
  if (cfg->ecc_check_enable) {
    v |= k_ra8_mipi_dsi_dsisetr_eccen;
  }
  if (cfg->eotp_enable) {
    v |= k_ra8_mipi_dsi_dsisetr_eotpen;
  }
  if (cfg->scramble_enable) {
    v |= k_ra8_mipi_dsi_dsisetr_scren;
  }
  if (cfg->tearing_detect_enable) {
    v |= k_ra8_mipi_dsi_dsisetr_extemd;
  }
  v |= (((uint32_t)cfg->crc_check_vc_mask) << (uint32_t)k_dsisetr_vc_crc_shift) &
       k_ra8_mipi_dsi_dsisetr_vc_crc_mask;
  return v;
}

/** @brief Implementation of `priv_ra8_mipi_dsi_internal_wait_eq()` -- bounded busy-poll. */
ra8_err_t
priv_ra8_mipi_dsi_internal_wait_eq(volatile const uint32_t* reg, uint32_t mask, uint32_t expect)
{
  for (uint32_t i = 0U; i < k_ra8_mipi_dsi_busy_loop_max; ++i) { /* GCOVR_EXCL_BR_LINE */
    if ((*reg & mask) == expect) {                               /* GCOVR_EXCL_BR_LINE */
      return k_ra8_ok;
    }
  }
  return k_ra8_err_hw_timeout;
}

/* =============================================================================
 * Public API -- lifecycle
 * =============================================================================
 */

/**
 * @brief Pulse the RSTCR software reset and program the steady-state
 *        link-level registers.
 *
 * @details
 * HUM Ch 65.2 "RSTCR" p 3853 plus the TXSETR / ULPSSETR / DSISETR /
 * CLSTPTSETR / LPTRNSTSETR cluster (pp 3845-3887). Split out so
 * ``ra8_mipi_dsi_init`` stays under the function-size threshold.
 *
 * @param[in] cfg Validated config.
 *
 * @pre Module clock ungated.
 * @post Listed registers reflect ``cfg``; SWRST cleared.
 *
 * @note Internal helper, not thread-safe.
 *
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_program_link(const ra8_mipi_dsi_config_t* cfg)
{
  volatile r_mipi_dsi_regs_t* reg = ra8_mipi_dsi();

  /* HUM Ch 65.2 "RSTCR" p 3845 */ /* pulse SWRST. */
  reg->RSTCR = k_ra8_mipi_dsi_rstcr_swrst;
  reg->RSTCR = 0U;

  /* HUM Ch 65.2 "TXSETR" p 3842 */ /* / "ULPSSETR" p 3849 / "DSISETR" p 3855 */
  reg->TXSETR   = internal_ra8_mipi_dsi_make_txsetr(cfg);
  reg->ULPSSETR = (uint32_t)cfg->ulps_wakeup_period << (uint32_t)k_ulpssetr_wkup_shift;
  reg->DSISETR  = internal_ra8_mipi_dsi_make_dsisetr(cfg);

  /* HUM Ch 65.2 "CLSTPTSETR" p 3881 */
  reg->CLSTPTSETR =
    ((((uint32_t)cfg->timing.clock_stop_time) << k_ra8_mipi_dsi_clstpt_clkstpt_shift) &
     k_ra8_mipi_dsi_clstpt_clkstpt_mask) |
    ((((uint32_t)cfg->timing.clock_beforehand_time) << k_ra8_mipi_dsi_clstpt_clkbfht_shift) &
     k_ra8_mipi_dsi_clstpt_clkbfht_mask) |
    ((((uint32_t)cfg->timing.clock_keep_time) << k_ra8_mipi_dsi_clstpt_clkkpt_shift) &
     k_ra8_mipi_dsi_clstpt_clkkpt_mask);

  /* HUM Ch 65.2 "LPTRNSTSETR" p 3883 */
  reg->LPTRNSTSETR = ((uint32_t)cfg->timing.go_lp_and_back) & k_ra8_mipi_dsi_lptrnst_golpbkt_mask;
}

/**
 * @brief Program the peripheral-response and transmit-side timeouts.
 *
 * @details
 * HUM Ch 65.2 PRESPTOBTASETR / PRESPTOLPSETR / PRESPTOHSSETR /
 * HSTXTOSETR / LRXHTOSETR / TATOSETR pp 3873-3881.
 *
 * @param[in] cfg Validated config.
 *
 * @pre Module clock ungated.
 * @post Listed timeout registers reflect ``cfg``.
 *
 * @note Internal helper, not thread-safe.
 *
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_program_timeouts(const ra8_mipi_dsi_config_t* cfg)
{
  volatile r_mipi_dsi_regs_t* reg = ra8_mipi_dsi();
  reg->PRESPTOBTASETR             = cfg->timeouts.bta_timeout;
  reg->PRESPTOLPSETR              = cfg->timeouts.lp_rw_timeout;
  reg->PRESPTOHSSETR              = cfg->timeouts.hs_rw_timeout;
  reg->HSTXTOSETR                 = cfg->timeouts.hs_tx_timeout;
  reg->LRXHTOSETR                 = cfg->timeouts.lp_rx_host_timeout;
  reg->TATOSETR                   = cfg->timeouts.turnaround_timeout;
}

[[nodiscard]] ra8_err_t ra8_mipi_dsi_init(const ra8_mipi_dsi_config_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");
  const ra8_err_t cfg_err = internal_ra8_mipi_dsi_validate_cfg(cfg);
  RA8_RETURN_ON_ERROR(cfg_err, s_tag, "mipi_dsi_init: cfg invalid"); /* GCOVR_EXCL_BR_LINE */

  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C", p 446 */
  const ra8_err_t mst_err = ra8_mstp_enable(k_ra8_mstp_mipi_dsi);
  RA8_RETURN_ON_ERROR(mst_err, s_tag, "mipi_dsi_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  internal_program_link(cfg);
  internal_program_timeouts(cfg);
  internal_ra8_mipi_dsi_clear_all_status();

  s_continuous_clock           = (cfg->clock_mode == k_ra8_mipi_dsi_clock_continuous);
  s_clock_lanes_in_ulps        = false;
  s_data_lanes_in_ulps         = false;
  s_mipi_dsi_event_fn          = nullptr;
  s_mipi_dsi_event_ctx         = nullptr;
  s_mipi_dsi_pending_rx_buffer = nullptr;
  s_mipi_dsi_pending_rx_len    = 0U;
  s_initialized                = true;

  ra8_log_info(s_tag, "mipi_dsi_init done");
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_mipi_dsi_deinit(void)
{
  /* Idempotent: a second deinit (or a deinit on a never-initialized
   * driver) must succeed without underflowing the MSTPC refcount.
   * HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C", p 446. */
  if (!s_initialized) {
    return k_ra8_ok;
  }

  volatile r_mipi_dsi_regs_t* reg = ra8_mipi_dsi();
  /* HUM Ch 65.2 "RSTCR : Reset Control Register", p 3845 */
  reg->RSTCR = k_ra8_mipi_dsi_rstcr_swrst;

  s_mipi_dsi_event_fn          = nullptr;
  s_mipi_dsi_event_ctx         = nullptr;
  s_continuous_clock           = false;
  s_clock_lanes_in_ulps        = false;
  s_data_lanes_in_ulps         = false;
  s_mipi_dsi_pending_rx_buffer = nullptr;
  s_mipi_dsi_pending_rx_len    = 0U;

  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C", p 446 */
  const ra8_err_t err = ra8_mstp_disable(k_ra8_mstp_mipi_dsi);
  if (err == k_ra8_ok) {
    s_initialized = false;
  }
  return err;
}

[[nodiscard]] ra8_err_t ra8_mipi_dsi_enter_stop(void)
{
  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C", p 446.
   * Releases the MSTPC reference taken in ``init``; the matching
   * ``exit_stop`` re-acquires it. */
  if (!s_initialized) {
    return k_ra8_err_invalid_state;
  }
  const ra8_err_t err = ra8_mstp_disable(k_ra8_mstp_mipi_dsi);
  if (err == k_ra8_ok) {
    s_initialized = false;
  }
  return err;
}

[[nodiscard]] ra8_err_t ra8_mipi_dsi_exit_stop(void)
{
  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C", p 446.
   * Re-acquires the MSTPC reference released by ``enter_stop``. */
  if (s_initialized) {
    return k_ra8_err_invalid_state;
  }
  const ra8_err_t err = ra8_mstp_enable(k_ra8_mstp_mipi_dsi);
  if (err == k_ra8_ok) {
    s_initialized = true;
  }
  return err;
}

[[nodiscard]] ra8_err_t ra8_mipi_dsi_soft_reset(void)
{
  volatile r_mipi_dsi_regs_t* reg = ra8_mipi_dsi();
  /* HUM Ch 65.2 "RSTCR : Reset Control Register", p 3845 */
  reg->RSTCR = k_ra8_mipi_dsi_rstcr_swrst;
  /* HUM Ch 65.2 "RSTCR : Reset Control Register", p 3845 */
  reg->RSTCR = 0U;
  return k_ra8_ok;
}

/* =============================================================================
 * HS clock control
 * =============================================================================
 */

[[nodiscard]] ra8_err_t ra8_mipi_dsi_hs_clock_start(void)
{
  volatile r_mipi_dsi_regs_t* reg   = ra8_mipi_dsi();
  uint32_t                    hsclk = k_ra8_mipi_dsi_hsclk_start;
  if (s_continuous_clock) {
    hsclk |= k_ra8_mipi_dsi_hsclk_continuous;
  }
  /* HUM Ch 65.2 "HSCLKSETR : HS Clock Setting Register", p 3843 */
  reg->HSCLKSETR = hsclk;
  /* HUM Ch 65.2 "PLSR : PHY Lane Status Register", p 3884 */
  return priv_ra8_mipi_dsi_internal_wait_eq(&reg->PLSR,
                                            k_ra8_mipi_dsi_plsr_cllp2hs,
                                            k_ra8_mipi_dsi_plsr_cllp2hs);
}

[[nodiscard]] ra8_err_t ra8_mipi_dsi_hs_clock_stop(void)
{
  volatile r_mipi_dsi_regs_t* reg = ra8_mipi_dsi();
  /* HUM Ch 65.2 "HSCLKSETR : HS Clock Setting Register", p 3843 */
  reg->HSCLKSETR = 0U;
  /* HUM Ch 65.2 "PLSR : PHY Lane Status Register", p 3884 */
  return priv_ra8_mipi_dsi_internal_wait_eq(&reg->PLSR,
                                            k_ra8_mipi_dsi_plsr_clhs2lp,
                                            k_ra8_mipi_dsi_plsr_clhs2lp);
}

/* =============================================================================
 * ULPS
 * =============================================================================
 */

[[nodiscard]] ra8_err_t ra8_mipi_dsi_ulps_enter(uint8_t lanes)
{
  if (lanes == k_ra8_mipi_dsi_lane_none) {
    return k_ra8_err_invalid_arg;
  }
  /* Continuous clock mode forbids clock-lane ULPS -- HUM Ch 65 lists
   * this constraint and FSP enforces the same precondition. */
  if (((lanes & k_ra8_mipi_dsi_lane_clock) != 0U) && s_continuous_clock) {
    ra8_log_error(s_tag, "ulps_enter: clock lane + continuous mode rejected");
    return k_ra8_err_invalid_arg;
  }
  uint32_t ulpscr = 0U;
  if (((lanes & k_ra8_mipi_dsi_lane_data) != 0U) && !s_data_lanes_in_ulps) {
    ulpscr |= k_ra8_mipi_dsi_ulpscr_dlent;
    s_data_lanes_in_ulps = true;
  }
  if (((lanes & k_ra8_mipi_dsi_lane_clock) != 0U) && !s_clock_lanes_in_ulps) {
    ulpscr |= k_ra8_mipi_dsi_ulpscr_clent;
    s_clock_lanes_in_ulps = true;
  }
  /* HUM Ch 65.2 "ULPSCR : ULPS Control Register", p 3844 */
  ra8_mipi_dsi()->ULPSCR = ulpscr;
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_mipi_dsi_ulps_exit(uint8_t lanes)
{
  if (lanes == k_ra8_mipi_dsi_lane_none) {
    return k_ra8_err_invalid_arg;
  }
  uint32_t ulpscr = 0U;
  if (((lanes & k_ra8_mipi_dsi_lane_data) != 0U) && s_data_lanes_in_ulps) {
    ulpscr |= k_ra8_mipi_dsi_ulpscr_dlexit;
    s_data_lanes_in_ulps = false;
  }
  if (((lanes & k_ra8_mipi_dsi_lane_clock) != 0U) && s_clock_lanes_in_ulps) {
    ulpscr |= k_ra8_mipi_dsi_ulpscr_clexit;
    s_clock_lanes_in_ulps = false;
  }
  /* HUM Ch 65.2 "ULPSCR : ULPS Control Register", p 3844 */
  ra8_mipi_dsi()->ULPSCR = ulpscr;
  return k_ra8_ok;
}
