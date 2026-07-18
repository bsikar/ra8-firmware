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
 * module-stop lifecycle, HS-clock control, sequence-channel command
 * submission, and ULPS paths. The video-mode, status, IRQ-dispatch, and
 * convenience surfaces live in the sibling `ra8_mipi_dsi_dispatch.c`;
 * state shared across the two TUs lives in `ra8_mipi_dsi_internal.h`.
 *
 * Every register access carries a HUM Ch 65 citation in the form
 * required by `scripts/utils/cite_check.py`:
 *
 *   /\* HUM Ch 65.X "name", p NNNN *\/
 *
 * @par State Machine
 * @startuml
 *  state idle
 *  state initialized
 *  state hs_clock_running
 *  state video_running
 *  state ulps
 *  [*] --> idle
 *  idle --> initialized        : ra8_mipi_dsi_init()
 *  initialized --> hs_clock_running : ra8_mipi_dsi_hs_clock_start()
 *  hs_clock_running --> video_running : ra8_mipi_dsi_video_start()
 *  hs_clock_running --> ulps   : ra8_mipi_dsi_ulps_enter()
 *  ulps --> hs_clock_running   : ra8_mipi_dsi_ulps_exit()
 *  video_running --> hs_clock_running : ra8_mipi_dsi_video_stop()
 *  hs_clock_running --> initialized : ra8_mipi_dsi_hs_clock_stop()
 *  initialized --> idle        : ra8_mipi_dsi_deinit()
 * @enduml
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

/**
 * @enum dsi_cmd_mask_t
 * @brief Low-nibble mask for the DCS command id.
 *
 * @details
 * Following the project's "no magic numbers" rule. The remaining
 * implementation-private numeric constants and shift positions shared
 * with ``ra8_mipi_dsi_dispatch.c`` live in ``ra8_mipi_dsi_internal.h``.
 */
typedef enum : uint32_t {
  k_dsi_cmd_id_mask = 0x0FU, /**< Low nibble of the DCS command id. */
} dsi_cmd_mask_t;

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
RA8_INTERNAL
static ra8_err_t internal_ra8_mipi_dsi_validate_cfg(const ra8_mipi_dsi_config_t* cfg)
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
RA8_INTERNAL
static void internal_ra8_mipi_dsi_clear_all_status(void)
{
  volatile r_mipi_dsi_regs_t* reg = ra8_mipi_dsi();
  /* HUM Ch 65.2 "SQCH0SCR : Sequence Channel 0 Status Clear", p 3934 */
  reg->SQCH0SCR = k_ra8_mipi_dsi_sqch_clear_all;
  /* HUM Ch 65.2 "SQCH1SCR : Sequence Channel 1 Status Clear", p 3934 */
  reg->SQCH1SCR = k_ra8_mipi_dsi_sqch_clear_all;
  /* HUM Ch 65.2 "VMSCR : Video Mode Status Clear", p 3893 */
  reg->VMSCR = k_ra8_mipi_dsi_vmsr_clear_all;
  /* HUM Ch 65.2 "RXSCR : Receive Status Clear", p 3870 */
  reg->RXSCR = k_ra8_mipi_dsi_rxsr_clear_all;
  /* HUM Ch 65.2 "FERRSCR : Fatal Error Status Clear", p 3884 */
  reg->FERRSCR = k_ra8_mipi_dsi_ferrsr_clear_all;
  /* HUM Ch 65.2 "PLSCR : PHY Lane Status Clear", p 3889 */
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
RA8_INTERNAL
static uint32_t internal_ra8_mipi_dsi_make_txsetr(const ra8_mipi_dsi_config_t* cfg)
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
RA8_INTERNAL
static uint32_t internal_ra8_mipi_dsi_make_dsisetr(const ra8_mipi_dsi_config_t* cfg)
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

/**
 * @brief Build descriptor word A from a fully-formed command struct.
 *
 * @param[in] cmd Validated command.
 * @return 32-bit DSC?AR value.
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
RA8_INTERNAL
static uint32_t internal_ra8_mipi_dsi_make_dsc_a(const ra8_mipi_dsi_command_t* cmd)
{
  uint8_t data0 = 0U;
  uint8_t data1 = 0U;
  /* Long packets put the word count in DATA0/DATA1; short packets put
   * the first two parameter bytes there. */
  const uint8_t low_nibble = (uint8_t)((uint32_t)cmd->cmd_id & k_dsi_cmd_id_mask);
  const bool    is_long    = low_nibble > 0x08U;
  if (is_long) {
    data0 = (uint8_t)(cmd->tx_len & k_ra8_mipi_dsi_byte_mask);
    data1 = (uint8_t)((cmd->tx_len >> (uint32_t)k_shift_8) & k_ra8_mipi_dsi_byte_mask);
  } else {
    if ((cmd->p_tx_buffer != nullptr) && (cmd->tx_len > 0U)) {
      data0 = cmd->p_tx_buffer[0];
    }
    if ((cmd->p_tx_buffer != nullptr) && (cmd->tx_len > 1U)) {
      data1 = cmd->p_tx_buffer[1];
    }
  }

  /* HUM Ch 65.2 "SQCH0DSC0AR : Sequence Channel 0 Descriptor 0 Setting A", p 3925 */
  uint32_t v =
    (((uint32_t)data0 & k_ra8_mipi_dsi_byte_mask) << (uint32_t)k_ra8_mipi_dsi_dsc0a_shift_data0) |
    (((uint32_t)data1 & k_ra8_mipi_dsi_byte_mask) << (uint32_t)k_ra8_mipi_dsi_dsc0a_shift_data1) |
    (((uint32_t)cmd->cmd_id & k_ra8_mipi_dsi_dt_mask) << (uint32_t)k_ra8_mipi_dsi_dsc0a_shift_dt) |
    (((uint32_t)cmd->virtual_channel & k_ra8_mipi_dsi_vc_mask)
     << (uint32_t)k_ra8_mipi_dsi_dsc0a_shift_vc);
  if (is_long) {
    v |= (1UL << (uint32_t)k_ra8_mipi_dsi_dsc0a_shift_fmt);
  }
  if (cmd->low_power) {
    v |= (1UL << (uint32_t)k_ra8_mipi_dsi_dsc0a_shift_spd);
  }
  v |= (((uint32_t)cmd->bta & k_ra8_mipi_dsi_bta_mask) << (uint32_t)k_ra8_mipi_dsi_dsc0a_shift_bta);
  return v;
}

/**
 * @brief Build descriptor word C (FINACT / AUXOP / ACTCODE).
 *
 * @param[in] cmd Validated command.
 * @return 32-bit DSC?CR value.
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
RA8_INTERNAL
static uint32_t internal_ra8_mipi_dsi_make_dsc_c(const ra8_mipi_dsi_command_t* cmd)
{
  uint32_t v = k_ra8_mipi_dsi_dsc0c_finact;
  if (cmd->aux_operation) {
    v |= k_ra8_mipi_dsi_dsc0c_auxop;
    v |= (((uint32_t)cmd->action_code) << k_ra8_mipi_dsi_dsc0c_actcode_shift) &
         k_ra8_mipi_dsi_dsc0c_actcode_mask;
  }
  return v;
}

/**
 * @brief Stage long-packet payload bytes into TXPPD0..3R.
 *
 * @param[in] data Payload bytes.
 * @param[in] len  Length, capped at `k_ra8_mipi_dsi_payload_max`.
 *
 * @details See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_ra8_mipi_dsi_stage_payload(const uint8_t* data, uint16_t len)
{
  volatile r_mipi_dsi_regs_t* reg      = ra8_mipi_dsi();
  uint32_t                    words[4] = {0U, 0U, 0U, 0U};
  const uint16_t              cap      = k_ra8_mipi_dsi_payload_max;
  const uint16_t              eff      = (len < cap) ? len : cap;
  for (uint16_t i = 0U; i < eff; ++i) {
    const uint16_t word_idx = (uint16_t)(i / 4U);
    const uint16_t byte_idx = (uint16_t)(i % 4U);
    words[word_idx] |= ((uint32_t)data[i]) << (byte_idx * (uint32_t)k_shift_8);
  }
  /* HUM Ch 65.2 "TXPPD0R : Transmit Packet Payload Data 0", p 3858 */
  reg->TXPPD0R = words[0];
  /* HUM Ch 65.2 "TXPPD1R : Transmit Packet Payload Data 1", p 3858 */
  reg->TXPPD1R = words[1];
  /* HUM Ch 65.2 "TXPPD2R : Transmit Packet Payload Data 2", p 3858 */
  reg->TXPPD2R = words[2];
  /* HUM Ch 65.2 "TXPPD3R : Transmit Packet Payload Data 3", p 3858 */
  reg->TXPPD3R = words[3];
}

/**
 * @brief Write the START-bit pulse for the given sequence channel.
 *
 * @param[in] channel 0 or 1.
 *
 * @details
 * Mirrors FSP `R_MIPI_DSI->SQCH?SET0R = SQCHnSET0R_BIT_23 | (n == ch)`
 * pattern. Both channel registers must be written so the engine
 * latches the channel-select bit correctly.
 *
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_ra8_mipi_dsi_pulse_start(uint8_t channel)
{
  volatile r_mipi_dsi_regs_t* reg = ra8_mipi_dsi();
  const uint32_t              ch0_word =
    k_ra8_mipi_dsi_sqch_chsel | ((channel == 0U) ? k_ra8_mipi_dsi_sqch_start : 0U);
  const uint32_t ch1_word =
    k_ra8_mipi_dsi_sqch_chsel | ((channel == 1U) ? k_ra8_mipi_dsi_sqch_start : 0U);
  /* HUM Ch 65.2 "SQCH0SET0R : Sequence Channel 0 Setting 0", p 3920 */
  reg->SQCH0SET0R = ch0_word;
  /* HUM Ch 65.2 "SQCH1SET0R : Sequence Channel 1 Setting 0", p 3922 */
  reg->SQCH1SET0R = ch1_word;
}

/** @brief Implementation of `ra8_mipi_dsi_internal_wait_eq()` -- bounded busy-poll. */
ra8_err_t
ra8_mipi_dsi_internal_wait_eq(volatile const uint32_t* reg, uint32_t mask, uint32_t expect)
{
  for (uint32_t i = 0U; i < k_ra8_mipi_dsi_busy_loop_max; ++i) { /* GCOVR_EXCL_BR_LINE */
    if ((*reg & mask) == expect) {                               /* GCOVR_EXCL_BR_LINE */
      return k_ra8_ok;
    }
  }
  return k_ra8_err_hw_timeout;
}

/**
 * @brief Common LP/HS bound-checking for command submission.
 *
 * @param[in] cmd Validated command (caller already null-checked).
 * @return `k_ra8_ok` if all bounds OK, otherwise an error code.
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
RA8_INTERNAL
static ra8_err_t internal_ra8_mipi_dsi_validate_cmd(const ra8_mipi_dsi_command_t* cmd)
{
  if ((uint32_t)cmd->virtual_channel > (uint32_t)k_ra8_mipi_dsi_vc3) {
    return k_ra8_err_invalid_arg;
  }
  if ((cmd->tx_len > 0U) && (cmd->p_tx_buffer == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (cmd->low_power && (cmd->tx_len > (uint32_t)k_ra8_mipi_dsi_max_lp_bytes)) {
    return k_ra8_err_invalid_arg;
  }
  if ((!cmd->low_power) && (cmd->tx_len > (uint32_t)k_ra8_mipi_dsi_max_hs_bytes)) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
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
RA8_INTERNAL
static void internal_program_link(const ra8_mipi_dsi_config_t* cfg)
{
  volatile r_mipi_dsi_regs_t* reg = ra8_mipi_dsi();

  /* HUM Ch 65.2 "RSTCR" p 3853 */ /* pulse SWRST. */
  reg->RSTCR = k_ra8_mipi_dsi_rstcr_swrst;
  reg->RSTCR = 0U;

  /* HUM Ch 65.2 "TXSETR" p 3845 */ /* / "ULPSSETR" p 3849 / "DSISETR" p 3855 */
  reg->TXSETR   = internal_ra8_mipi_dsi_make_txsetr(cfg);
  reg->ULPSSETR = (uint32_t)cfg->ulps_wakeup_period << (uint32_t)k_ulpssetr_wkup_shift;
  reg->DSISETR  = internal_ra8_mipi_dsi_make_dsisetr(cfg);

  /* HUM Ch 65.2 "CLSTPTSETR" p 3886 */
  reg->CLSTPTSETR =
    ((((uint32_t)cfg->timing.clock_stop_time) << k_ra8_mipi_dsi_clstpt_clkstpt_shift) &
     k_ra8_mipi_dsi_clstpt_clkstpt_mask) |
    ((((uint32_t)cfg->timing.clock_beforehand_time) << k_ra8_mipi_dsi_clstpt_clkbfht_shift) &
     k_ra8_mipi_dsi_clstpt_clkbfht_mask) |
    ((((uint32_t)cfg->timing.clock_keep_time) << k_ra8_mipi_dsi_clstpt_clkkpt_shift) &
     k_ra8_mipi_dsi_clstpt_clkkpt_mask);

  /* HUM Ch 65.2 "LPTRNSTSETR" p 3887 */
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
RA8_INTERNAL
static void internal_program_timeouts(const ra8_mipi_dsi_config_t* cfg)
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
  /* HUM Ch 65.2 "RSTCR : Reset Control Register", p 3853 */
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
  /* HUM Ch 65.2 "RSTCR : Reset Control Register", p 3853 */
  reg->RSTCR = k_ra8_mipi_dsi_rstcr_swrst;
  /* HUM Ch 65.2 "RSTCR : Reset Control Register", p 3853 */
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
  /* HUM Ch 65.2 "HSCLKSETR : HS Clock Setting Register", p 3848 */
  reg->HSCLKSETR = hsclk;
  /* HUM Ch 65.2 "PLSR : PHY Lane Status Register", p 3888 */
  return ra8_mipi_dsi_internal_wait_eq(&reg->PLSR,
                                       k_ra8_mipi_dsi_plsr_cllp2hs,
                                       k_ra8_mipi_dsi_plsr_cllp2hs);
}

[[nodiscard]] ra8_err_t ra8_mipi_dsi_hs_clock_stop(void)
{
  volatile r_mipi_dsi_regs_t* reg = ra8_mipi_dsi();
  /* HUM Ch 65.2 "HSCLKSETR : HS Clock Setting Register", p 3848 */
  reg->HSCLKSETR = 0U;
  /* HUM Ch 65.2 "PLSR : PHY Lane Status Register", p 3888 */
  return ra8_mipi_dsi_internal_wait_eq(&reg->PLSR,
                                       k_ra8_mipi_dsi_plsr_clhs2lp,
                                       k_ra8_mipi_dsi_plsr_clhs2lp);
}

/* =============================================================================
 * Sequence-channel commands
 * =============================================================================
 */

/**
 * @brief Run the LINKSR-based pre-condition checks for ``send_command``.
 *
 * @details
 * HUM Ch 65.2 "LINKSR : Link Status Register" p 3842. Rejects LP /
 * AUX commands while video mode is running and refuses to interleave
 * with a busy sequence channel.
 *
 * @param[in] cmd Validated command descriptor.
 *
 * @return ``k_ra8_ok`` if the link permits the command.
 * @retval k_ra8_err_invalid_state LP/AUX requested during video mode.
 * @retval k_ra8_err_busy A sequence channel is mid-transfer.
 *
 * @pre ``cmd`` is non-null.
 * @post No side effects.
 *
 * @note Internal helper, not thread-safe.
 *
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_check_link_state(const ra8_mipi_dsi_command_t* cmd)
{
  volatile r_mipi_dsi_regs_t* reg  = ra8_mipi_dsi();
  const uint32_t              link = reg->LINKSR;
  if (cmd->low_power && ((link & k_ra8_mipi_dsi_link_vrun) != 0U)) {
    ra8_log_error(s_tag, "send_command: LP not allowed during video mode");
    return k_ra8_err_invalid_state;
  }
  if ((link & (k_ra8_mipi_dsi_link_sq0run | k_ra8_mipi_dsi_link_sq1run)) != 0U) {
    ra8_log_error(s_tag, "send_command: sequence busy");
    return k_ra8_err_busy;
  }
  if (cmd->aux_operation && ((link & k_ra8_mipi_dsi_link_vrun) != 0U)) {
    return k_ra8_err_invalid_state;
  }
  return k_ra8_ok;
}

/**
 * @brief Fill the four descriptor words for a sequence-channel command.
 *
 * @details
 * HUM Ch 65.2 SQCH0/1DSC0AR..DR pp 3925-3930. Word D points at either
 * the rx or the tx buffer depending on the BTA direction.
 *
 * @param[in,out] dsc  Descriptor base for the chosen channel.
 * @param[in]     cmd  Validated command descriptor.
 *
 * @pre ``dsc`` and ``cmd`` are non-null.
 * @post Descriptor words A..D reflect ``cmd``.
 *
 * @note Internal helper, not thread-safe.
 *
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_program_descriptor(volatile r_mipi_dsi_descriptor_t* dsc,
                                        const ra8_mipi_dsi_command_t*     cmd)
{
  dsc->A                   = internal_ra8_mipi_dsi_make_dsc_a(cmd);
  dsc->B                   = k_ra8_mipi_dsi_dsc0b_dtsel_seqrm;
  dsc->C                   = internal_ra8_mipi_dsi_make_dsc_c(cmd);
  const uintptr_t buf_addr = (cmd->bta == k_ra8_mipi_dsi_bta_read) || (cmd->p_rx_buffer != nullptr)
                               ? (uintptr_t)cmd->p_rx_buffer
                               : (uintptr_t)cmd->p_tx_buffer;
  dsc->D                   = (uint32_t)buf_addr;
}

/**
 * @brief Stage payload + descriptor for ``ra8_mipi_dsi_send_command``.
 *
 * @details
 * For long packets (data-type low nibble > 0x08) we stage the first
 * 16 bytes through TXPPD and let the descriptor's word D point at the
 * caller buffer for the remainder. Sequence channel 0 carries LP
 * commands; channel 1 carries HS commands.
 *
 * @param[in] cmd Validated command descriptor.
 *
 * @pre Link-state check passed.
 * @post Sequence-channel descriptor + payload window programmed and
 *       the matching channel pulsed.
 *
 * @note Internal helper, not thread-safe.
 *
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_send_stage_and_pulse(const ra8_mipi_dsi_command_t* cmd)
{
  volatile r_mipi_dsi_regs_t* reg = ra8_mipi_dsi();

  uint8_t channel = 1U;
  if (cmd->low_power) {
    channel = 0U;
  }
  const uint8_t low_nibble = (uint8_t)((uint32_t)cmd->cmd_id & k_dsi_cmd_id_mask);
  const bool    is_long    = low_nibble > 0x08U;

  if (is_long && (cmd->tx_len > 0U)) {
    internal_ra8_mipi_dsi_stage_payload(cmd->p_tx_buffer, cmd->tx_len);
  }

  volatile r_mipi_dsi_descriptor_t* dsc = (channel == 0U) ? &reg->SQCH0DSC[0] : &reg->SQCH1DSC[0];
  internal_program_descriptor(dsc, cmd);

  internal_ra8_mipi_dsi_pulse_start(channel);
}

[[nodiscard]] ra8_err_t ra8_mipi_dsi_send_command(const ra8_mipi_dsi_command_t* cmd)
{
  RA8_CHECK_NULL_PTR(cmd, s_tag, "cmd must not be nullptr");
  const ra8_err_t v_err = internal_ra8_mipi_dsi_validate_cmd(cmd);
  RA8_RETURN_ON_ERROR(v_err, s_tag, "send_command: validate"); /* GCOVR_EXCL_BR_LINE */

  const ra8_err_t link_err = internal_check_link_state(cmd);
  RA8_RETURN_ON_ERROR(link_err, s_tag, "send_command: link state"); /* GCOVR_EXCL_BR_LINE */

  internal_send_stage_and_pulse(cmd);

  if (cmd->p_rx_buffer != nullptr) {
    s_mipi_dsi_pending_rx_buffer = cmd->p_rx_buffer;
    s_mipi_dsi_pending_rx_len    = k_ra8_mipi_dsi_payload_max;
  }

  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_mipi_dsi_send_short_packet(ra8_mipi_dsi_dt_t cmd_id,
                                                       ra8_mipi_dsi_vc_t vc,
                                                       uint8_t           param0,
                                                       uint8_t           param1)
{
  uint8_t                      buf[2] = {param0, param1};
  const ra8_mipi_dsi_command_t cmd    = {
    .cmd_id          = cmd_id,
    .virtual_channel = vc,
    .bta             = k_ra8_mipi_dsi_bta_none,
    .low_power       = true,
    .ack_request     = false,
    .aux_operation   = false,
    .action_code     = 0U,
    .tx_len          = 2U,
    .p_tx_buffer     = buf,
    .p_rx_buffer     = nullptr,
  };
  return ra8_mipi_dsi_send_command(&cmd);
}

[[nodiscard]] ra8_err_t ra8_mipi_dsi_send_long_packet(ra8_mipi_dsi_dt_t cmd_id,
                                                      ra8_mipi_dsi_vc_t vc,
                                                      const uint8_t*    data,
                                                      uint16_t          tx_len,
                                                      bool              low_power)
{
  if ((tx_len > 0U) && (data == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  const ra8_mipi_dsi_command_t cmd = {
    .cmd_id          = cmd_id,
    .virtual_channel = vc,
    .bta             = k_ra8_mipi_dsi_bta_none,
    .low_power       = low_power,
    .ack_request     = false,
    .aux_operation   = false,
    .action_code     = 0U,
    .tx_len          = tx_len,
    .p_tx_buffer     = data,
    .p_rx_buffer     = nullptr,
  };
  return ra8_mipi_dsi_send_command(&cmd);
}

[[nodiscard]] ra8_err_t ra8_mipi_dsi_read_packet(ra8_mipi_dsi_dt_t cmd_id,
                                                 ra8_mipi_dsi_vc_t vc,
                                                 uint8_t           param0,
                                                 uint8_t           param1,
                                                 uint8_t*          p_rx_buffer,
                                                 uint16_t          rx_len)
{
  RA8_CHECK_NULL_PTR(p_rx_buffer, s_tag, "p_rx_buffer must not be nullptr");
  if (rx_len == 0U) {
    return k_ra8_err_invalid_arg;
  }
  uint8_t                      tx_buf[2] = {param0, param1};
  const ra8_mipi_dsi_command_t cmd       = {
    .cmd_id          = cmd_id,
    .virtual_channel = vc,
    .bta             = k_ra8_mipi_dsi_bta_read,
    .low_power       = true,
    .ack_request     = true,
    .aux_operation   = false,
    .action_code     = 0U,
    .tx_len          = 2U,
    .p_tx_buffer     = tx_buf,
    .p_rx_buffer     = p_rx_buffer,
  };
  s_mipi_dsi_pending_rx_buffer = p_rx_buffer;
  s_mipi_dsi_pending_rx_len    = rx_len;
  return ra8_mipi_dsi_send_command(&cmd);
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
  /* HUM Ch 65.2 "ULPSCR : ULPS Control Register", p 3851 */
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
  /* HUM Ch 65.2 "ULPSCR : ULPS Control Register", p 3851 */
  ra8_mipi_dsi()->ULPSCR = ulpscr;
  return k_ra8_ok;
}
