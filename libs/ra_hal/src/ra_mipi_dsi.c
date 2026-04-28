/**
 * @file ra_mipi_dsi.c
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
 * The D-PHY analog block (HUM Ch 64) is owned by `ra_mipi_phy.c`. This
 * driver only programmes the DSI link/protocol layer above it.
 *
 * Every register access carries a HUM Ch 65 citation in the form
 * required by `scripts/utils/cite_check.py`:
 *
 *   /\* HUM Ch 65.X "name", p NNNN *\/
 *
 * @par State Machine
 * @startuml
 *  state idle
 *  state initialised
 *  state hs_clock_running
 *  state video_running
 *  state ulps
 *  [*] --> idle
 *  idle --> initialised        : ra_mipi_dsi_init()
 *  initialised --> hs_clock_running : ra_mipi_dsi_hs_clock_start()
 *  hs_clock_running --> video_running : ra_mipi_dsi_video_start()
 *  hs_clock_running --> ulps   : ra_mipi_dsi_ulps_enter()
 *  ulps --> hs_clock_running   : ra_mipi_dsi_ulps_exit()
 *  video_running --> hs_clock_running : ra_mipi_dsi_video_stop()
 *  hs_clock_running --> initialised : ra_mipi_dsi_hs_clock_stop()
 *  initialised --> idle        : ra_mipi_dsi_deinit()
 * @enduml
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_mipi_dsi.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8d2_mipi_dsi_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_mstp.h"

/**
 * @var s_tag
 * @brief Component tag used by the `ra_log_*` family.
 *
 * @details
 * Static so the linker keeps it confined to this TU. Same convention
 * as every other ra_hal driver (see `ra_glcdc.c`, `ra_doc.c`).
 *
 * @note Read-only after assignment. Not modified at runtime.
 * @warning Never modify directly -- declared `const` to enforce.
 * @since 0.1.0
 */
static const char* s_tag = "MIPI_DSI";

/**
 * @var s_dsi_fn
 * @brief Currently registered IRQ callback, or `nullptr`.
 *
 * @note Mutated only from single-threaded init / deinit /
 *       attach-handler call sites; read from IRQ context.
 * @warning Direct access from outside this TU is forbidden.
 * @since 0.1.0
 */
static ra_mipi_dsi_event_fn_t s_dsi_fn;

/**
 * @var s_dsi_ctx
 * @brief Opaque context handed back to the IRQ callback.
 *
 * @note Lifetime is owned by the caller of `ra_mipi_dsi_attach_handler`.
 * @warning May be NULL if the user never attached.
 * @since 0.1.0
 */
static void* s_dsi_ctx;

/**
 * @var s_continuous_clock
 * @brief Cache of `cfg->clock_mode == continuous` from the last init.
 *
 * @details
 * Needed because the ULPS-enter helper must reject a clock-lane ULPS
 * request when continuous-clock mode is on -- the FSP source enforces
 * the same rule. Snap-shotting it once at init avoids an extra
 * register read on every `ra_mipi_dsi_ulps_enter()` call.
 *
 * @note Mutated only from `ra_mipi_dsi_init()`.
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
 * @warning Reset by `ra_mipi_dsi_init()` and `_deinit()`.
 * @since 0.1.0
 */
static bool s_clock_lanes_in_ulps;

/**
 * @var s_data_lanes_in_ulps
 * @brief Software shadow of data-lane ULPS state.
 *
 * @note Updated only from ULPS enter / exit helpers.
 * @warning Reset by `ra_mipi_dsi_init()` and `_deinit()`.
 * @since 0.1.0
 */
static bool s_data_lanes_in_ulps;

/**
 * @var s_pending_rx_buffer
 * @brief Buffer caller passed to `ra_mipi_dsi_read_packet()`.
 *
 * @details
 * The receive ISR copies long-packet payload bytes from RXPPD0..3R
 * into this buffer once the response arrives. Cleared on completion
 * to avoid stale-pointer use.
 *
 * @note Modified only from read_packet + RX dispatch.
 * @warning The pointer must remain valid until the IRQ fires.
 * @since 0.1.0
 */
static uint8_t* s_pending_rx_buffer;

/**
 * @var s_pending_rx_len
 * @brief Capacity of `s_pending_rx_buffer` in bytes.
 *
 * @note Cleared to zero once the response is consumed.
 * @warning Treat as undefined when `s_pending_rx_buffer` is NULL.
 * @since 0.1.0
 */
static uint16_t s_pending_rx_len;

/**
 * @enum ra_mipi_dsi_internal_t
 * @brief Numeric constants used by the implementation that don't
 *        belong in the public header.
 *
 * @details
 * Following the project's "no magic numbers" rule -- every literal
 * embedded in the driver code path lives here as a typed enum value.
 */
typedef enum : uint32_t {
  k_ra_mipi_dsi_clear_all = 0xFFFFFFFFUL, /**< RW1C all-ones write mask. */
  k_ra_mipi_dsi_isr_all   = (uint32_t)k_ra_mipi_dsi_isr_sq0 | (uint32_t)k_ra_mipi_dsi_isr_sq1 |
                            (uint32_t)k_ra_mipi_dsi_isr_vm | (uint32_t)k_ra_mipi_dsi_isr_rcv |
                            (uint32_t)k_ra_mipi_dsi_isr_ferr | (uint32_t)k_ra_mipi_dsi_isr_ppi,
  k_ra_mipi_dsi_byte_mask = 0xFFUL, /**< 8-bit byte field.        */
  k_ra_mipi_dsi_vc_mask   = 0x3UL,  /**< 2-bit virtual-channel.   */
  k_ra_mipi_dsi_dt_mask   = 0x3FUL, /**< 6-bit DSI Data Type.     */
  k_ra_mipi_dsi_bta_mask  = 0x3UL,  /**< 2-bit BTA selector.      */
} ra_mipi_dsi_internal_t;

/**
 * @enum ra_mipi_dsi_shifts_t
 * @brief Common shift positions used in this TU.
 */
typedef enum : uint8_t {
  k_shift_8              = 8U,
  k_ulpssetr_wkup_shift  = 0U,
  k_dsisetr_mrpsz_shift  = 0U,
  k_dsisetr_vc_crc_shift = 20U,
  k_rxrss_data1_shift    = 8U,
  k_rxrss_dt_shift       = 16U,
  k_rxrss_vc_shift       = 22U,
  k_akep_vc_shift        = 16U,
} ra_mipi_dsi_shifts_t;

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/**
 * @brief Validate a `ra_mipi_dsi_config_t` argument.
 *
 * @param[in] cfg Caller-supplied config.
 * @return `k_ra_ok` if OK, `k_ra_err_invalid_arg` if `lane_count` is
 *         outside [1,2].
 *
 * @pre `cfg` is non-NULL (checked by caller via `RA_CHECK_NULL_PTR`).
 * @pre Caller is in single-threaded context.
 * @post No side effects.
 */
static ra_err_t internal_ra_mipi_dsi_validate_cfg(const ra_mipi_dsi_config_t* cfg)
{
  if ((cfg->lane_count != k_ra_mipi_dsi_lanes_1) && (cfg->lane_count != k_ra_mipi_dsi_lanes_2)) {
    return k_ra_err_invalid_arg;
  }
  return k_ra_ok;
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
 */
static void internal_ra_mipi_dsi_clear_all_status(void)
{
  volatile r_mipi_dsi_regs_t* reg = ra_mipi_dsi();
  /* HUM Ch 65.2 "SQCH0SCR : Sequence Channel 0 Status Clear", p 3934 */
  reg->SQCH0SCR = (uint32_t)k_ra_mipi_dsi_sqch_clear_all;
  /* HUM Ch 65.2 "SQCH1SCR : Sequence Channel 1 Status Clear", p 3934 */
  reg->SQCH1SCR = (uint32_t)k_ra_mipi_dsi_sqch_clear_all;
  /* HUM Ch 65.2 "VMSCR : Video Mode Status Clear", p 3893 */
  reg->VMSCR = (uint32_t)k_ra_mipi_dsi_vmsr_clear_all;
  /* HUM Ch 65.2 "RXSCR : Receive Status Clear", p 3870 */
  reg->RXSCR = (uint32_t)k_ra_mipi_dsi_rxsr_clear_all;
  /* HUM Ch 65.2 "FERRSCR : Fatal Error Status Clear", p 3884 */
  reg->FERRSCR = (uint32_t)k_ra_mipi_dsi_ferrsr_clear_all;
  /* HUM Ch 65.2 "PLSCR : PHY Lane Status Clear", p 3889 */
  reg->PLSCR = (uint32_t)k_ra_mipi_dsi_plsr_clear_all;
}

/**
 * @brief Compose `TXSETR` from a config (lane count + clock + data lane enable).
 *
 * @param[in] cfg Validated configuration.
 * @return Word ready for write to `TXSETR`.
 */
static uint32_t internal_ra_mipi_dsi_make_txsetr(const ra_mipi_dsi_config_t* cfg)
{
  uint32_t v = (uint32_t)k_ra_mipi_dsi_txset_clen | (uint32_t)k_ra_mipi_dsi_txset_dlen;
  if (cfg->lane_count == k_ra_mipi_dsi_lanes_2) {
    v |= (uint32_t)k_ra_mipi_dsi_txset_lane2;
  }
  return v;
}

/**
 * @brief Compose `DSISETR` from a config.
 *
 * @param[in] cfg Validated configuration.
 * @return Word ready for write to `DSISETR`.
 */
static uint32_t internal_ra_mipi_dsi_make_dsisetr(const ra_mipi_dsi_config_t* cfg)
{
  uint32_t v = ((uint32_t)cfg->max_return_packet_size & (uint32_t)k_ra_mipi_dsi_dsisetr_mrpsz_mask);
  if (cfg->ecc_check_enable) {
    v |= (uint32_t)k_ra_mipi_dsi_dsisetr_eccen;
  }
  if (cfg->eotp_enable) {
    v |= (uint32_t)k_ra_mipi_dsi_dsisetr_eotpen;
  }
  if (cfg->scramble_enable) {
    v |= (uint32_t)k_ra_mipi_dsi_dsisetr_scren;
  }
  if (cfg->tearing_detect_enable) {
    v |= (uint32_t)k_ra_mipi_dsi_dsisetr_extemd;
  }
  v |= (((uint32_t)cfg->crc_check_vc_mask) << (uint32_t)k_dsisetr_vc_crc_shift) &
       (uint32_t)k_ra_mipi_dsi_dsisetr_vc_crc_mask;
  return v;
}

/**
 * @brief Build descriptor word A from a fully-formed command struct.
 *
 * @param[in] cmd Validated command.
 * @return 32-bit DSC?AR value.
 */
static uint32_t internal_ra_mipi_dsi_make_dsc_a(const ra_mipi_dsi_command_t* cmd)
{
  uint8_t data0 = 0U;
  uint8_t data1 = 0U;
  /* Long packets put the word count in DATA0/DATA1; short packets put
   * the first two parameter bytes there. */
  const uint8_t low_nibble = (uint8_t)((uint32_t)cmd->cmd_id & 0x0FU);
  const bool    is_long    = (low_nibble > 0x08U);
  if (is_long) {
    data0 = (uint8_t)(cmd->tx_len & (uint32_t)k_ra_mipi_dsi_byte_mask);
    data1 = (uint8_t)((cmd->tx_len >> (uint32_t)k_shift_8) & (uint32_t)k_ra_mipi_dsi_byte_mask);
  } else {
    if ((cmd->p_tx_buffer != nullptr) && (cmd->tx_len > 0U)) {
      data0 = cmd->p_tx_buffer[0];
    }
    if ((cmd->p_tx_buffer != nullptr) && (cmd->tx_len > 1U)) {
      data1 = cmd->p_tx_buffer[1];
    }
  }

  /* HUM Ch 65.2 "SQCH0DSC0AR : Sequence Channel 0 Descriptor 0 Setting A", p 3925 */
  uint32_t v = (((uint32_t)data0 & (uint32_t)k_ra_mipi_dsi_byte_mask)
                << (uint32_t)k_ra_mipi_dsi_dsc0a_shift_data0) |
               (((uint32_t)data1 & (uint32_t)k_ra_mipi_dsi_byte_mask)
                << (uint32_t)k_ra_mipi_dsi_dsc0a_shift_data1) |
               (((uint32_t)cmd->cmd_id & (uint32_t)k_ra_mipi_dsi_dt_mask)
                << (uint32_t)k_ra_mipi_dsi_dsc0a_shift_dt) |
               (((uint32_t)cmd->virtual_channel & (uint32_t)k_ra_mipi_dsi_vc_mask)
                << (uint32_t)k_ra_mipi_dsi_dsc0a_shift_vc);
  if (is_long) {
    v |= (1UL << (uint32_t)k_ra_mipi_dsi_dsc0a_shift_fmt);
  }
  if (cmd->low_power) {
    v |= (1UL << (uint32_t)k_ra_mipi_dsi_dsc0a_shift_spd);
  }
  v |= (((uint32_t)cmd->bta & (uint32_t)k_ra_mipi_dsi_bta_mask)
        << (uint32_t)k_ra_mipi_dsi_dsc0a_shift_bta);
  return v;
}

/**
 * @brief Build descriptor word C (FINACT / AUXOP / ACTCODE).
 *
 * @param[in] cmd Validated command.
 * @return 32-bit DSC?CR value.
 */
static uint32_t internal_ra_mipi_dsi_make_dsc_c(const ra_mipi_dsi_command_t* cmd)
{
  uint32_t v = (uint32_t)k_ra_mipi_dsi_dsc0c_finact;
  if (cmd->aux_operation) {
    v |= (uint32_t)k_ra_mipi_dsi_dsc0c_auxop;
    v |= (((uint32_t)cmd->action_code) << (uint32_t)k_ra_mipi_dsi_dsc0c_actcode_shift) &
         (uint32_t)k_ra_mipi_dsi_dsc0c_actcode_mask;
  }
  return v;
}

/**
 * @brief Stage long-packet payload bytes into TXPPD0..3R.
 *
 * @param[in] data Payload bytes.
 * @param[in] len  Length, capped at `k_ra_mipi_dsi_payload_max`.
 */
static void internal_ra_mipi_dsi_stage_payload(const uint8_t* data, uint16_t len)
{
  volatile r_mipi_dsi_regs_t* reg      = ra_mipi_dsi();
  uint32_t                    words[4] = {0U, 0U, 0U, 0U};
  const uint16_t              cap      = (uint16_t)k_ra_mipi_dsi_payload_max;
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
 */
static void internal_ra_mipi_dsi_pulse_start(uint8_t channel)
{
  volatile r_mipi_dsi_regs_t* reg = ra_mipi_dsi();
  const uint32_t ch0_word         = (uint32_t)k_ra_mipi_dsi_sqch_chsel |
                                    ((channel == 0U) ? (uint32_t)k_ra_mipi_dsi_sqch_start : 0U);
  const uint32_t ch1_word         = (uint32_t)k_ra_mipi_dsi_sqch_chsel |
                                    ((channel == 1U) ? (uint32_t)k_ra_mipi_dsi_sqch_start : 0U);
  /* HUM Ch 65.2 "SQCH0SET0R : Sequence Channel 0 Setting 0", p 3920 */
  reg->SQCH0SET0R = ch0_word;
  /* HUM Ch 65.2 "SQCH1SET0R : Sequence Channel 1 Setting 0", p 3922 */
  reg->SQCH1SET0R = ch1_word;
}

/**
 * @brief Decode an RXRSS slot register into a struct.
 *
 * @param[in]  raw       Raw 32-bit RXRSS?R word.
 * @param[out] out_result Populated by the caller.
 */
static void internal_ra_mipi_dsi_decode_rx(uint32_t raw, ra_mipi_dsi_rx_result_t* out_result)
{
  out_result->data[0] = (uint8_t)(raw & (uint32_t)k_ra_mipi_dsi_rxrss_data0_mask);
  out_result->data[1] =
    (uint8_t)((raw & (uint32_t)k_ra_mipi_dsi_rxrss_data1_mask) >> (uint32_t)k_rxrss_data1_shift);
  out_result->cmd_id =
    (ra_mipi_dsi_dt_t)((raw & (uint32_t)k_ra_mipi_dsi_rxrss_dt_mask) >> (uint32_t)k_rxrss_dt_shift);
  out_result->virtual_channel =
    (ra_mipi_dsi_vc_t)((raw & (uint32_t)k_ra_mipi_dsi_rxrss_vc_mask) >> (uint32_t)k_rxrss_vc_shift);
  out_result->long_packet          = ((raw & (uint32_t)k_ra_mipi_dsi_rxrss_fmt) != 0U);
  out_result->rx_success           = ((raw & (uint32_t)k_ra_mipi_dsi_rxrss_rxsuc) != 0U);
  out_result->rx_fatal_error       = ((raw & (uint32_t)k_ra_mipi_dsi_rxrss_rxferr) != 0U);
  out_result->rx_fail              = ((raw & (uint32_t)k_ra_mipi_dsi_rxrss_rxfail) != 0U);
  out_result->rx_packet_data_fail  = ((raw & (uint32_t)k_ra_mipi_dsi_rxrss_rxpfail) != 0U);
  out_result->rx_correctable_error = ((raw & (uint32_t)k_ra_mipi_dsi_rxrss_rxcerr) != 0U);
  out_result->rx_ack_and_error     = ((raw & (uint32_t)k_ra_mipi_dsi_rxrss_rxake) != 0U);
  out_result->info_overwritten     = ((raw & (uint32_t)k_ra_mipi_dsi_rxrss_infoow) != 0U);
}

/**
 * @brief Bounded poll waiting for `(reg & mask) == expect`.
 *
 * @param[in] reg    Address of the register to poll.
 * @param[in] mask   Bits to test.
 * @param[in] expect Required value of `reg & mask`.
 * @return `k_ra_ok` if the condition was met within
 *         `k_ra_mipi_dsi_busy_loop_max` iterations, otherwise
 *         `k_ra_err_hw_timeout`.
 */
static ra_err_t
internal_ra_mipi_dsi_wait_eq(volatile const uint32_t* reg, uint32_t mask, uint32_t expect)
{
  for (uint32_t i = 0U; i < (uint32_t)k_ra_mipi_dsi_busy_loop_max; ++i) {
    if ((*reg & mask) == expect) {
      return k_ra_ok;
    }
  }
  return k_ra_err_hw_timeout;
}

/**
 * @brief Common LP/HS bound-checking for command submission.
 *
 * @param[in] cmd Validated command (caller already null-checked).
 * @return `k_ra_ok` if all bounds OK, otherwise an error code.
 */
static ra_err_t internal_ra_mipi_dsi_validate_cmd(const ra_mipi_dsi_command_t* cmd)
{
  if ((uint32_t)cmd->virtual_channel > (uint32_t)k_ra_mipi_dsi_vc3) {
    return k_ra_err_invalid_arg;
  }
  if ((cmd->tx_len > 0U) && (cmd->p_tx_buffer == nullptr)) {
    return k_ra_err_null_ptr;
  }
  if (cmd->low_power && (cmd->tx_len > (uint32_t)k_ra_mipi_dsi_max_lp_bytes)) {
    return k_ra_err_invalid_arg;
  }
  if ((!cmd->low_power) && (cmd->tx_len > (uint32_t)k_ra_mipi_dsi_max_hs_bytes)) {
    return k_ra_err_invalid_arg;
  }
  return k_ra_ok;
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
 * ``ra_mipi_dsi_init`` stays under the function-size threshold.
 *
 * @param[in] cfg Validated config.
 *
 * @pre Module clock ungated.
 * @post Listed registers reflect ``cfg``; SWRST cleared.
 *
 * @note Internal helper, not thread-safe.
 */
static void internal_program_link(const ra_mipi_dsi_config_t* cfg)
{
  volatile r_mipi_dsi_regs_t* reg = ra_mipi_dsi();

  /* HUM Ch 65.2 "RSTCR" p 3853: pulse SWRST. */
  reg->RSTCR = (uint32_t)k_ra_mipi_dsi_rstcr_swrst;
  reg->RSTCR = 0U;

  /* HUM Ch 65.2 "TXSETR" p 3845 / "ULPSSETR" p 3849 / "DSISETR" p 3855 */
  reg->TXSETR   = internal_ra_mipi_dsi_make_txsetr(cfg);
  reg->ULPSSETR = (uint32_t)cfg->ulps_wakeup_period << (uint32_t)k_ulpssetr_wkup_shift;
  reg->DSISETR  = internal_ra_mipi_dsi_make_dsisetr(cfg);

  /* HUM Ch 65.2 "CLSTPTSETR" p 3886 */
  reg->CLSTPTSETR =
    ((((uint32_t)cfg->timing.clock_stop_time) << (uint32_t)k_ra_mipi_dsi_clstpt_clkstpt_shift) &
     (uint32_t)k_ra_mipi_dsi_clstpt_clkstpt_mask) |
    ((((uint32_t)cfg->timing.clock_beforehand_time)
      << (uint32_t)k_ra_mipi_dsi_clstpt_clkbfht_shift) &
     (uint32_t)k_ra_mipi_dsi_clstpt_clkbfht_mask) |
    ((((uint32_t)cfg->timing.clock_keep_time) << (uint32_t)k_ra_mipi_dsi_clstpt_clkkpt_shift) &
     (uint32_t)k_ra_mipi_dsi_clstpt_clkkpt_mask);

  /* HUM Ch 65.2 "LPTRNSTSETR" p 3887 */
  reg->LPTRNSTSETR =
    ((uint32_t)cfg->timing.go_lp_and_back) & (uint32_t)k_ra_mipi_dsi_lptrnst_golpbkt_mask;
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
 */
static void internal_program_timeouts(const ra_mipi_dsi_config_t* cfg)
{
  volatile r_mipi_dsi_regs_t* reg = ra_mipi_dsi();
  reg->PRESPTOBTASETR             = cfg->timeouts.bta_timeout;
  reg->PRESPTOLPSETR              = cfg->timeouts.lp_rw_timeout;
  reg->PRESPTOHSSETR              = cfg->timeouts.hs_rw_timeout;
  reg->HSTXTOSETR                 = cfg->timeouts.hs_tx_timeout;
  reg->LRXHTOSETR                 = cfg->timeouts.lp_rx_host_timeout;
  reg->TATOSETR                   = cfg->timeouts.turnaround_timeout;
}

[[nodiscard]] ra_err_t ra_mipi_dsi_init(const ra_mipi_dsi_config_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");
  const ra_err_t cfg_err = internal_ra_mipi_dsi_validate_cfg(cfg);
  RA_RETURN_ON_ERROR(cfg_err, s_tag, "mipi_dsi_init: cfg invalid"); /* GCOVR_EXCL_BR_LINE */

  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C", p 446 */
  const ra_err_t mst_err = ra_mstp_enable(k_ra_mstp_mipi_dsi);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "mipi_dsi_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  internal_program_link(cfg);
  internal_program_timeouts(cfg);
  internal_ra_mipi_dsi_clear_all_status();

  s_continuous_clock    = (cfg->clock_mode == k_ra_mipi_dsi_clock_continuous);
  s_clock_lanes_in_ulps = false;
  s_data_lanes_in_ulps  = false;
  s_dsi_fn              = nullptr;
  s_dsi_ctx             = nullptr;
  s_pending_rx_buffer   = nullptr;
  s_pending_rx_len      = 0U;

  ra_log_info(s_tag, "mipi_dsi_init done");
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_mipi_dsi_deinit(void)
{
  volatile r_mipi_dsi_regs_t* reg = ra_mipi_dsi();
  /* HUM Ch 65.2 "RSTCR : Reset Control Register", p 3853 */
  reg->RSTCR = (uint32_t)k_ra_mipi_dsi_rstcr_swrst;

  s_dsi_fn              = nullptr;
  s_dsi_ctx             = nullptr;
  s_continuous_clock    = false;
  s_clock_lanes_in_ulps = false;
  s_data_lanes_in_ulps  = false;
  s_pending_rx_buffer   = nullptr;
  s_pending_rx_len      = 0U;

  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C", p 446 */
  return ra_mstp_disable(k_ra_mstp_mipi_dsi);
}

[[nodiscard]] ra_err_t ra_mipi_dsi_enter_stop(void)
{
  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C", p 446 */
  return ra_mstp_disable(k_ra_mstp_mipi_dsi);
}

[[nodiscard]] ra_err_t ra_mipi_dsi_exit_stop(void)
{
  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C", p 446 */
  return ra_mstp_enable(k_ra_mstp_mipi_dsi);
}

[[nodiscard]] ra_err_t ra_mipi_dsi_soft_reset(void)
{
  volatile r_mipi_dsi_regs_t* reg = ra_mipi_dsi();
  /* HUM Ch 65.2 "RSTCR : Reset Control Register", p 3853 */
  reg->RSTCR = (uint32_t)k_ra_mipi_dsi_rstcr_swrst;
  /* HUM Ch 65.2 "RSTCR : Reset Control Register", p 3853 */
  reg->RSTCR = 0U;
  return k_ra_ok;
}

/* =============================================================================
 * HS clock control
 * =============================================================================
 */

[[nodiscard]] ra_err_t ra_mipi_dsi_hs_clock_start(void)
{
  volatile r_mipi_dsi_regs_t* reg   = ra_mipi_dsi();
  uint32_t                    hsclk = (uint32_t)k_ra_mipi_dsi_hsclk_start;
  if (s_continuous_clock) {
    hsclk |= (uint32_t)k_ra_mipi_dsi_hsclk_continuous;
  }
  /* HUM Ch 65.2 "HSCLKSETR : HS Clock Setting Register", p 3848 */
  reg->HSCLKSETR = hsclk;
  /* HUM Ch 65.2 "PLSR : PHY Lane Status Register", p 3888 */
  return internal_ra_mipi_dsi_wait_eq(&reg->PLSR,
                                      (uint32_t)k_ra_mipi_dsi_plsr_cllp2hs,
                                      (uint32_t)k_ra_mipi_dsi_plsr_cllp2hs);
}

[[nodiscard]] ra_err_t ra_mipi_dsi_hs_clock_stop(void)
{
  volatile r_mipi_dsi_regs_t* reg = ra_mipi_dsi();
  /* HUM Ch 65.2 "HSCLKSETR : HS Clock Setting Register", p 3848 */
  reg->HSCLKSETR = 0U;
  /* HUM Ch 65.2 "PLSR : PHY Lane Status Register", p 3888 */
  return internal_ra_mipi_dsi_wait_eq(&reg->PLSR,
                                      (uint32_t)k_ra_mipi_dsi_plsr_clhs2lp,
                                      (uint32_t)k_ra_mipi_dsi_plsr_clhs2lp);
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
 * @return ``k_ra_ok`` if the link permits the command.
 * @retval k_ra_err_invalid_state LP/AUX requested during video mode.
 * @retval k_ra_err_busy A sequence channel is mid-transfer.
 *
 * @pre ``cmd`` is non-null.
 * @post No side effects.
 *
 * @note Internal helper, not thread-safe.
 */
static ra_err_t internal_check_link_state(const ra_mipi_dsi_command_t* cmd)
{
  volatile r_mipi_dsi_regs_t* reg  = ra_mipi_dsi();
  const uint32_t              link = reg->LINKSR;
  if (cmd->low_power && ((link & (uint32_t)k_ra_mipi_dsi_link_vrun) != 0U)) {
    ra_log_error(s_tag, "send_command: LP not allowed during video mode");
    return k_ra_err_invalid_state;
  }
  if ((link & ((uint32_t)k_ra_mipi_dsi_link_sq0run | (uint32_t)k_ra_mipi_dsi_link_sq1run)) != 0U) {
    ra_log_error(s_tag, "send_command: sequence busy");
    return k_ra_err_busy;
  }
  if (cmd->aux_operation && ((link & (uint32_t)k_ra_mipi_dsi_link_vrun) != 0U)) {
    return k_ra_err_invalid_state;
  }
  return k_ra_ok;
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
 */
static void internal_program_descriptor(volatile r_mipi_dsi_descriptor_t* dsc,
                                        const ra_mipi_dsi_command_t*      cmd)
{
  dsc->A                   = internal_ra_mipi_dsi_make_dsc_a(cmd);
  dsc->B                   = (uint32_t)k_ra_mipi_dsi_dsc0b_dtsel_seqrm;
  dsc->C                   = internal_ra_mipi_dsi_make_dsc_c(cmd);
  const uintptr_t buf_addr = (cmd->bta == k_ra_mipi_dsi_bta_read) || (cmd->p_rx_buffer != nullptr)
                               ? (uintptr_t)cmd->p_rx_buffer
                               : (uintptr_t)cmd->p_tx_buffer;
  dsc->D                   = (uint32_t)buf_addr;
}

/**
 * @brief Stage payload + descriptor for ``ra_mipi_dsi_send_command``.
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
 */
static void internal_send_stage_and_pulse(const ra_mipi_dsi_command_t* cmd)
{
  volatile r_mipi_dsi_regs_t* reg = ra_mipi_dsi();

  uint8_t channel = 1U;
  if (cmd->low_power) {
    channel = 0U;
  }
  const uint8_t low_nibble = (uint8_t)((uint32_t)cmd->cmd_id & 0x0FU);
  const bool    is_long    = (low_nibble > 0x08U);

  if (is_long && (cmd->tx_len > 0U)) {
    internal_ra_mipi_dsi_stage_payload(cmd->p_tx_buffer, cmd->tx_len);
  }

  volatile r_mipi_dsi_descriptor_t* dsc = (channel == 0U) ? &reg->SQCH0DSC[0] : &reg->SQCH1DSC[0];
  internal_program_descriptor(dsc, cmd);

  internal_ra_mipi_dsi_pulse_start(channel);
}

[[nodiscard]] ra_err_t ra_mipi_dsi_send_command(const ra_mipi_dsi_command_t* cmd)
{
  RA_CHECK_NULL_PTR(cmd, s_tag, "cmd must not be nullptr");
  const ra_err_t v_err = internal_ra_mipi_dsi_validate_cmd(cmd);
  RA_RETURN_ON_ERROR(v_err, s_tag, "send_command: validate"); /* GCOVR_EXCL_BR_LINE */

  const ra_err_t link_err = internal_check_link_state(cmd);
  RA_RETURN_ON_ERROR(link_err, s_tag, "send_command: link state"); /* GCOVR_EXCL_BR_LINE */

  internal_send_stage_and_pulse(cmd);

  if (cmd->p_rx_buffer != nullptr) {
    s_pending_rx_buffer = cmd->p_rx_buffer;
    s_pending_rx_len    = (uint16_t)k_ra_mipi_dsi_payload_max;
  }

  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_mipi_dsi_send_short_packet(ra_mipi_dsi_dt_t cmd_id,
                                                     ra_mipi_dsi_vc_t vc,
                                                     uint8_t          param0,
                                                     uint8_t          param1)
{
  uint8_t                     buf[2] = {param0, param1};
  const ra_mipi_dsi_command_t cmd    = {
    .cmd_id          = cmd_id,
    .virtual_channel = vc,
    .bta             = k_ra_mipi_dsi_bta_none,
    .low_power       = true,
    .ack_request     = false,
    .aux_operation   = false,
    .action_code     = 0U,
    .tx_len          = 2U,
    .p_tx_buffer     = buf,
    .p_rx_buffer     = nullptr,
  };
  return ra_mipi_dsi_send_command(&cmd);
}

[[nodiscard]] ra_err_t ra_mipi_dsi_send_long_packet(ra_mipi_dsi_dt_t cmd_id,
                                                    ra_mipi_dsi_vc_t vc,
                                                    const uint8_t*   data,
                                                    uint16_t         tx_len,
                                                    bool             low_power)
{
  if ((tx_len > 0U) && (data == nullptr)) {
    return k_ra_err_null_ptr;
  }
  const ra_mipi_dsi_command_t cmd = {
    .cmd_id          = cmd_id,
    .virtual_channel = vc,
    .bta             = k_ra_mipi_dsi_bta_none,
    .low_power       = low_power,
    .ack_request     = false,
    .aux_operation   = false,
    .action_code     = 0U,
    .tx_len          = tx_len,
    .p_tx_buffer     = data,
    .p_rx_buffer     = nullptr,
  };
  return ra_mipi_dsi_send_command(&cmd);
}

[[nodiscard]] ra_err_t ra_mipi_dsi_read_packet(ra_mipi_dsi_dt_t cmd_id,
                                               ra_mipi_dsi_vc_t vc,
                                               uint8_t          param0,
                                               uint8_t          param1,
                                               uint8_t*         p_rx_buffer,
                                               uint16_t         rx_len)
{
  RA_CHECK_NULL_PTR(p_rx_buffer, s_tag, "p_rx_buffer must not be nullptr");
  if (rx_len == 0U) {
    return k_ra_err_invalid_arg;
  }
  uint8_t                     tx_buf[2] = {param0, param1};
  const ra_mipi_dsi_command_t cmd       = {
    .cmd_id          = cmd_id,
    .virtual_channel = vc,
    .bta             = k_ra_mipi_dsi_bta_read,
    .low_power       = true,
    .ack_request     = true,
    .aux_operation   = false,
    .action_code     = 0U,
    .tx_len          = 2U,
    .p_tx_buffer     = tx_buf,
    .p_rx_buffer     = p_rx_buffer,
  };
  s_pending_rx_buffer = p_rx_buffer;
  s_pending_rx_len    = rx_len;
  return ra_mipi_dsi_send_command(&cmd);
}

/* =============================================================================
 * ULPS
 * =============================================================================
 */

[[nodiscard]] ra_err_t ra_mipi_dsi_ulps_enter(uint8_t lanes)
{
  if (lanes == (uint8_t)k_ra_mipi_dsi_lane_none) {
    return k_ra_err_invalid_arg;
  }
  /* Continuous clock mode forbids clock-lane ULPS -- HUM Ch 65 lists
   * this constraint and FSP enforces the same precondition. */
  if (((lanes & (uint8_t)k_ra_mipi_dsi_lane_clock) != 0U) && s_continuous_clock) {
    ra_log_error(s_tag, "ulps_enter: clock lane + continuous mode rejected");
    return k_ra_err_invalid_arg;
  }
  uint32_t ulpscr = 0U;
  if (((lanes & (uint8_t)k_ra_mipi_dsi_lane_data) != 0U) && !s_data_lanes_in_ulps) {
    ulpscr |= (uint32_t)k_ra_mipi_dsi_ulpscr_dlent;
    s_data_lanes_in_ulps = true;
  }
  if (((lanes & (uint8_t)k_ra_mipi_dsi_lane_clock) != 0U) && !s_clock_lanes_in_ulps) {
    ulpscr |= (uint32_t)k_ra_mipi_dsi_ulpscr_clent;
    s_clock_lanes_in_ulps = true;
  }
  /* HUM Ch 65.2 "ULPSCR : ULPS Control Register", p 3851 */
  ra_mipi_dsi()->ULPSCR = ulpscr;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_mipi_dsi_ulps_exit(uint8_t lanes)
{
  if (lanes == (uint8_t)k_ra_mipi_dsi_lane_none) {
    return k_ra_err_invalid_arg;
  }
  uint32_t ulpscr = 0U;
  if (((lanes & (uint8_t)k_ra_mipi_dsi_lane_data) != 0U) && s_data_lanes_in_ulps) {
    ulpscr |= (uint32_t)k_ra_mipi_dsi_ulpscr_dlexit;
    s_data_lanes_in_ulps = false;
  }
  if (((lanes & (uint8_t)k_ra_mipi_dsi_lane_clock) != 0U) && s_clock_lanes_in_ulps) {
    ulpscr |= (uint32_t)k_ra_mipi_dsi_ulpscr_clexit;
    s_clock_lanes_in_ulps = false;
  }
  /* HUM Ch 65.2 "ULPSCR : ULPS Control Register", p 3851 */
  ra_mipi_dsi()->ULPSCR = ulpscr;
  return k_ra_ok;
}

/* =============================================================================
 * Video mode
 * =============================================================================
 */

[[nodiscard]] ra_err_t ra_mipi_dsi_video_configure(const ra_mipi_dsi_video_cfg_t* vcfg)
{
  RA_CHECK_NULL_PTR(vcfg, s_tag, "vcfg must not be nullptr");
  if ((uint32_t)vcfg->virtual_channel > (uint32_t)k_ra_mipi_dsi_vc3) {
    return k_ra_err_invalid_arg;
  }

  volatile r_mipi_dsi_regs_t* reg = ra_mipi_dsi();

  /* HUM Ch 65.2 "VMSET1R : Video Mode Setting 1", p 3892 */
  reg->VMSET1R = (((uint32_t)vcfg->video_mode_delay) << (uint32_t)k_ra_mipi_dsi_vmset1_dly_shift) &
                 (uint32_t)k_ra_mipi_dsi_vmset1_dly_mask;

  /* HUM Ch 65.2 "VMPPSETR : Video Mode Pixel Packet Setting", p 3895 */
  uint32_t vmpp = (((uint32_t)vcfg->pixel_format) << (uint32_t)k_ra_mipi_dsi_vmpp_dt_shift) &
                  (uint32_t)k_ra_mipi_dsi_vmpp_dt_mask;
  vmpp |= (((uint32_t)vcfg->virtual_channel) << (uint32_t)k_ra_mipi_dsi_vmpp_vc_shift) &
          (uint32_t)k_ra_mipi_dsi_vmpp_vc_mask;
  if (vcfg->sync_pulse) {
    vmpp |= (uint32_t)k_ra_mipi_dsi_vmpp_txesync;
  }
  reg->VMPPSETR = vmpp;

  /* HUM Ch 65.2 "VMVSSETR : Video Mode Vertical Sync Setting", p 3897 */
  uint32_t vmvs = (((uint32_t)vcfg->vertical_sync_lines) & (uint32_t)k_ra_mipi_dsi_vmvs_vsa_mask);
  vmvs |= (((uint32_t)vcfg->vertical_active_lines) << (uint32_t)k_ra_mipi_dsi_vmvs_vact_shift) &
          (uint32_t)k_ra_mipi_dsi_vmvs_vact_mask;
  if (vcfg->vsync_active_high) {
    vmvs |= (uint32_t)k_ra_mipi_dsi_vmvs_vspol;
  }
  reg->VMVSSETR = vmvs;

  /* HUM Ch 65.2 "VMVPSETR : Video Mode Vertical Porch Setting", p 3898 */
  uint32_t vmvp = ((uint32_t)vcfg->vertical_back_porch) & (uint32_t)k_ra_mipi_dsi_vmvp_vbp_mask;
  vmvp |= (((uint32_t)vcfg->vertical_front_porch) << (uint32_t)k_ra_mipi_dsi_vmvp_vfp_shift) &
          (uint32_t)k_ra_mipi_dsi_vmvp_vfp_mask;
  reg->VMVPSETR = vmvp;

  /* HUM Ch 65.2 "VMHSSETR : Video Mode Horizontal Sync Setting", p 3899 */
  uint32_t vmhs = ((uint32_t)vcfg->horizontal_sync_lines) & (uint32_t)k_ra_mipi_dsi_vmhs_hsa_mask;
  vmhs |= (((uint32_t)vcfg->horizontal_active_pixels) << (uint32_t)k_ra_mipi_dsi_vmhs_hact_shift) &
          (uint32_t)k_ra_mipi_dsi_vmhs_hact_mask;
  if (vcfg->hsync_active_high) {
    vmhs |= (uint32_t)k_ra_mipi_dsi_vmhs_hspol;
  }
  reg->VMHSSETR = vmhs;

  /* HUM Ch 65.2 "VMHPSETR : Video Mode Horizontal Porch Setting", p 3900 */
  uint32_t vmhp = ((uint32_t)vcfg->horizontal_back_porch) & (uint32_t)k_ra_mipi_dsi_vmhp_hbp_mask;
  vmhp |= (((uint32_t)vcfg->horizontal_front_porch) << (uint32_t)k_ra_mipi_dsi_vmhp_hfp_shift) &
          (uint32_t)k_ra_mipi_dsi_vmhp_hfp_mask;
  reg->VMHPSETR = vmhp;

  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_mipi_dsi_video_start(const ra_mipi_dsi_video_cfg_t* vcfg)
{
  RA_CHECK_NULL_PTR(vcfg, s_tag, "vcfg must not be nullptr");
  volatile r_mipi_dsi_regs_t* reg   = ra_mipi_dsi();
  uint32_t                    vmset = (uint32_t)k_ra_mipi_dsi_vmset0_vstart;
  if (vcfg->hsa_no_lp) {
    vmset |= (uint32_t)k_ra_mipi_dsi_vmset0_hsanolp;
  }
  if (vcfg->hbp_no_lp) {
    vmset |= (uint32_t)k_ra_mipi_dsi_vmset0_hbpnolp;
  }
  if (vcfg->hfp_no_lp) {
    vmset |= (uint32_t)k_ra_mipi_dsi_vmset0_hfpnolp;
  }
  /* HUM Ch 65.2 "VMSET0R : Video Mode Setting 0", p 3891 */
  reg->VMSET0R = vmset;
  /* HUM Ch 65.2 "VMSR : Video Mode Status Register", p 3893 */
  return internal_ra_mipi_dsi_wait_eq(&reg->VMSR,
                                      (uint32_t)k_ra_mipi_dsi_vmsr_virdy,
                                      (uint32_t)k_ra_mipi_dsi_vmsr_virdy);
}

[[nodiscard]] ra_err_t ra_mipi_dsi_video_stop(void)
{
  volatile r_mipi_dsi_regs_t* reg = ra_mipi_dsi();
  /* HUM Ch 65.2 "VMSET0R : Video Mode Setting 0", p 3891 */
  reg->VMSET0R = (uint32_t)k_ra_mipi_dsi_vmset0_vstop;
  /* HUM Ch 65.2 "VMSR : Video Mode Status Register", p 3893 */
  const ra_err_t err = internal_ra_mipi_dsi_wait_eq(&reg->VMSR,
                                                    (uint32_t)k_ra_mipi_dsi_vmsr_stop,
                                                    (uint32_t)k_ra_mipi_dsi_vmsr_stop);
  if (err == k_ra_ok) {
    /* HUM Ch 65.2 "VMSCR : Video Mode Status Clear", p 3893 */
    reg->VMSCR = (uint32_t)k_ra_mipi_dsi_vmsr_clear_all;
  }
  return err;
}

/* =============================================================================
 * Status / IRQ
 * =============================================================================
 */

[[nodiscard]] ra_err_t ra_mipi_dsi_get_status(uint32_t* out_mask)
{
  RA_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  /* HUM Ch 65.2 "ISR : Interrupt Status Register", p 3840 */
  *out_mask = ra_mipi_dsi()->ISR;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_mipi_dsi_link_status_get(ra_mipi_dsi_link_status_t* out_status)
{
  RA_CHECK_NULL_PTR(out_status, s_tag, "out_status must not be nullptr");
  /* HUM Ch 65.2 "LINKSR : Link Status Register", p 3842 */
  const uint32_t v                 = ra_mipi_dsi()->LINKSR;
  out_status->sequence_ch0_running = ((v & (uint32_t)k_ra_mipi_dsi_link_sq0run) != 0U);
  out_status->sequence_ch1_running = ((v & (uint32_t)k_ra_mipi_dsi_link_sq1run) != 0U);
  out_status->video_running        = ((v & (uint32_t)k_ra_mipi_dsi_link_vrun) != 0U);
  out_status->hs_busy              = ((v & (uint32_t)k_ra_mipi_dsi_link_hsbusy) != 0U);
  out_status->lp_busy              = ((v & (uint32_t)k_ra_mipi_dsi_link_lpbusy) != 0U);
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_mipi_dsi_clear_status(uint32_t mask)
{
  volatile r_mipi_dsi_regs_t* reg = ra_mipi_dsi();
  if ((mask & (uint32_t)k_ra_mipi_dsi_isr_sq0) != 0U) {
    /* HUM Ch 65.2 "SQCH0SCR : Sequence Channel 0 Status Clear", p 3934 */
    reg->SQCH0SCR = (uint32_t)k_ra_mipi_dsi_sqch_clear_all;
  }
  if ((mask & (uint32_t)k_ra_mipi_dsi_isr_sq1) != 0U) {
    /* HUM Ch 65.2 "SQCH1SCR : Sequence Channel 1 Status Clear", p 3934 */
    reg->SQCH1SCR = (uint32_t)k_ra_mipi_dsi_sqch_clear_all;
  }
  if ((mask & (uint32_t)k_ra_mipi_dsi_isr_vm) != 0U) {
    /* HUM Ch 65.2 "VMSCR : Video Mode Status Clear", p 3893 */
    reg->VMSCR = (uint32_t)k_ra_mipi_dsi_vmsr_clear_all;
  }
  if ((mask & (uint32_t)k_ra_mipi_dsi_isr_rcv) != 0U) {
    /* HUM Ch 65.2 "RXSCR : Receive Status Clear", p 3870 */
    reg->RXSCR = (uint32_t)k_ra_mipi_dsi_rxsr_clear_all;
  }
  if ((mask & (uint32_t)k_ra_mipi_dsi_isr_ferr) != 0U) {
    /* HUM Ch 65.2 "FERRSCR : Fatal Error Status Clear", p 3884 */
    reg->FERRSCR = (uint32_t)k_ra_mipi_dsi_ferrsr_clear_all;
  }
  if ((mask & (uint32_t)k_ra_mipi_dsi_isr_ppi) != 0U) {
    /* HUM Ch 65.2 "PLSCR : PHY Lane Status Clear", p 3889 */
    reg->PLSCR = (uint32_t)k_ra_mipi_dsi_plsr_clear_all;
  }
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_mipi_dsi_ack_error_get(ra_mipi_dsi_ack_error_t* out_err)
{
  RA_CHECK_NULL_PTR(out_err, s_tag, "out_err must not be nullptr");
  volatile r_mipi_dsi_regs_t* reg = ra_mipi_dsi();
  /* HUM Ch 65.2 "AKEPACMSR : Ack/Error Report Accumulated Status", p 3877 */
  const uint32_t v      = reg->AKEPACMSR;
  out_err->error_report = (uint16_t)(v & (uint32_t)k_ra_mipi_dsi_akep_erep_mask);
  out_err->virtual_channel =
    (ra_mipi_dsi_vc_t)(((v & (uint32_t)k_ra_mipi_dsi_akep_vc_mask) >> (uint32_t)k_akep_vc_shift) &
                       (uint32_t)k_ra_mipi_dsi_vc_mask);
  /* HUM Ch 65.2 "AKEPSCR : Ack/Error Report Status Clear", p 3878 */
  reg->AKEPSCR = v;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_mipi_dsi_rx_result_get(uint8_t slot, ra_mipi_dsi_rx_result_t* out_result)
{
  RA_CHECK_NULL_PTR(out_result, s_tag, "out_result must not be nullptr");
  if (slot >= (uint8_t)k_ra_mipi_dsi_rx_slots) {
    return k_ra_err_invalid_arg;
  }
  volatile r_mipi_dsi_regs_t* reg = ra_mipi_dsi();
  /* HUM Ch 65.2 "RXRSSR : Receive Result Save Status Register", p 3878 */
  const uint32_t valid_bit = (1UL << slot);
  if ((reg->RXRSSR & valid_bit) == 0U) {
    return k_ra_err_no_data;
  }
  uint32_t raw = 0U;
  /* HUM Ch 65.2 "RXRSS0R..RXRSS3R : Receive Result Save 0..3", p 3879 */
  switch (slot) {
    case 0U:
      raw = reg->RXRSS0R;
      break;
    case 1U:
      raw = reg->RXRSS1R;
      break;
    case 2U:
      raw = reg->RXRSS2R;
      break;
    case 3U:
    default:
      raw = reg->RXRSS3R;
      break;
  }
  internal_ra_mipi_dsi_decode_rx(raw, out_result);
  /* HUM Ch 65.2 "RXRSSCR : Receive Result Save Status Clear", p 3878 */
  reg->RXRSSCR = valid_bit;
  /* HUM Ch 65.2 "RXRINFOOWSCR : Receive Result Info-Overwrite Clear", p 3879 */
  reg->RXRINFOOWSCR = valid_bit;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t
ra_mipi_dsi_rx_payload_read(uint8_t* dest, uint16_t max_len, uint16_t* out_len)
{
  RA_CHECK_NULL_PTR(dest, s_tag, "dest must not be nullptr");
  RA_CHECK_NULL_PTR(out_len, s_tag, "out_len must not be nullptr");
  volatile r_mipi_dsi_regs_t* reg = ra_mipi_dsi();
  /* HUM Ch 65.2 "RXPPD0R..RXPPD3R : Receive Packet Payload 0..3", p 3879 */
  const uint32_t words[4] = {reg->RXPPD0R, reg->RXPPD1R, reg->RXPPD2R, reg->RXPPD3R};
  const uint16_t cap      = (uint16_t)k_ra_mipi_dsi_payload_max;
  const uint16_t eff      = (max_len < cap) ? max_len : cap;
  for (uint16_t i = 0U; i < eff; ++i) {
    const uint16_t word_idx = (uint16_t)(i / 4U);
    const uint16_t byte_idx = (uint16_t)(i % 4U);
    dest[i]                 = (uint8_t)((words[word_idx] >> (byte_idx * (uint32_t)k_shift_8)) &
                                        (uint32_t)k_ra_mipi_dsi_byte_mask);
  }
  *out_len = eff;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_mipi_dsi_te_event_pending(bool* out_pending)
{
  RA_CHECK_NULL_PTR(out_pending, s_tag, "out_pending must not be nullptr");
  /* HUM Ch 65.2 "RXSR : Receive Status Register", p 3868 */
  const uint32_t v       = ra_mipi_dsi()->RXSR;
  const uint32_t te_mask = (uint32_t)k_ra_mipi_dsi_rxsr_rxte | (uint32_t)k_ra_mipi_dsi_rxsr_extedet;
  *out_pending           = ((v & te_mask) != 0U);
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_mipi_dsi_te_event_clear(void)
{
  /* HUM Ch 65.2 "RXSCR : Receive Status Clear", p 3870 */
  ra_mipi_dsi()->RXSCR = (uint32_t)k_ra_mipi_dsi_rxsr_rxte | (uint32_t)k_ra_mipi_dsi_rxsr_extedet;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_mipi_dsi_irq_enable(ra_mipi_dsi_event_t event, uint32_t mask, bool enable)
{
  volatile r_mipi_dsi_regs_t* reg = ra_mipi_dsi();
  volatile uint32_t*          ier = nullptr;
  switch (event) {
    case k_ra_mipi_dsi_event_seq0:
      /* HUM Ch 65.2 "SQCH0IER : Sequence Channel 0 Interrupt Enable", p 3934 */
      ier = &reg->SQCH0IER;
      break;
    case k_ra_mipi_dsi_event_seq1:
      /* HUM Ch 65.2 "SQCH1IER : Sequence Channel 1 Interrupt Enable", p 3934 */
      ier = &reg->SQCH1IER;
      break;
    case k_ra_mipi_dsi_event_video:
      /* HUM Ch 65.2 "VMIER : Video Mode Interrupt Enable", p 3893 */
      ier = &reg->VMIER;
      break;
    case k_ra_mipi_dsi_event_receive:
      /* HUM Ch 65.2 "RXIER : Receive Interrupt Enable", p 3870 */
      ier = &reg->RXIER;
      break;
    case k_ra_mipi_dsi_event_fatal:
      /* HUM Ch 65.2 "FERRIER : Fatal Error Interrupt Enable", p 3884 */
      ier = &reg->FERRIER;
      break;
    case k_ra_mipi_dsi_event_phy:
      /* HUM Ch 65.2 "PLIER : PHY Lane Interrupt Enable", p 3889 */
      ier = &reg->PLIER;
      break;
    default:
      return k_ra_err_invalid_arg;
  }
  if (enable) {
    *ier |= mask;
  } else {
    *ier &= ~mask;
  }
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_mipi_dsi_attach_handler(ra_mipi_dsi_event_fn_t fn, void* ctx)
{
  s_dsi_fn  = fn;
  s_dsi_ctx = ctx;
  return k_ra_ok;
}

/* =============================================================================
 * Per-class dispatch
 * =============================================================================
 */

/**
 * @brief Common helper that fires the registered callback.
 *
 * @param[in] event Class enum.
 * @param[in] mask  Status bits captured before clearing.
 */
static void internal_ra_mipi_dsi_call_user(ra_mipi_dsi_event_t event, uint32_t mask)
{
  const ra_mipi_dsi_event_fn_t fn  = s_dsi_fn;
  void* const                  ctx = s_dsi_ctx;
  if (fn != nullptr) {
    fn(ctx, event, mask);
  }
}

void ra_mipi_dsi_dispatch_seq0(void)
{
  volatile r_mipi_dsi_regs_t* reg = ra_mipi_dsi();
  /* HUM Ch 65.2 "SQCH0SR : Sequence Channel 0 Status Register", p 3933 */
  const uint32_t bits = reg->SQCH0SR;
  /* HUM Ch 65.2 "SQCH0SCR : Sequence Channel 0 Status Clear", p 3934 */
  reg->SQCH0SCR = bits & (uint32_t)k_ra_mipi_dsi_sqch_clear_all;
  internal_ra_mipi_dsi_call_user(k_ra_mipi_dsi_event_seq0, bits);
}

void ra_mipi_dsi_dispatch_seq1(void)
{
  volatile r_mipi_dsi_regs_t* reg = ra_mipi_dsi();
  /* HUM Ch 65.2 "SQCH1SR : Sequence Channel 1 Status Register", p 3933 */
  const uint32_t bits = reg->SQCH1SR;
  /* HUM Ch 65.2 "SQCH1SCR : Sequence Channel 1 Status Clear", p 3934 */
  reg->SQCH1SCR = bits & (uint32_t)k_ra_mipi_dsi_sqch_clear_all;
  internal_ra_mipi_dsi_call_user(k_ra_mipi_dsi_event_seq1, bits);
}

void ra_mipi_dsi_dispatch_video(void)
{
  volatile r_mipi_dsi_regs_t* reg = ra_mipi_dsi();
  /* HUM Ch 65.2 "VMSR : Video Mode Status Register", p 3893 */
  const uint32_t bits = reg->VMSR;
  /* HUM Ch 65.2 "VMSCR : Video Mode Status Clear", p 3893 */
  reg->VMSCR = bits & (uint32_t)k_ra_mipi_dsi_vmsr_clear_all;
  /* If buffer over/underflow, FSP recommends a soft reset; mirror that. */
  if ((bits & ((uint32_t)k_ra_mipi_dsi_vmsr_vbufovf | (uint32_t)k_ra_mipi_dsi_vmsr_vbufudf)) !=
      0U) {
    /* HUM Ch 65.2 "RSTCR : Reset Control Register", p 3853 */
    reg->RSTCR = (uint32_t)k_ra_mipi_dsi_rstcr_swrst;
    /* HUM Ch 65.2 "RSTCR : Reset Control Register", p 3853 */
    reg->RSTCR = 0U;
  }
  internal_ra_mipi_dsi_call_user(k_ra_mipi_dsi_event_video, bits);
}

void ra_mipi_dsi_dispatch_receive(void)
{
  volatile r_mipi_dsi_regs_t* reg = ra_mipi_dsi();
  /* HUM Ch 65.2 "RXSR : Receive Status Register", p 3868 */
  const uint32_t bits = reg->RXSR;
  /* HUM Ch 65.2 "RXSCR : Receive Status Clear", p 3870 */
  reg->RXSCR = bits & (uint32_t)k_ra_mipi_dsi_rxsr_clear_all;
  /* If a response packet arrived, copy RXPPD into the pending buffer. */
  if ((bits & (uint32_t)k_ra_mipi_dsi_rxsr_rxresp) != 0U) {
    if ((s_pending_rx_buffer != nullptr) && (s_pending_rx_len > 0U)) {
      uint16_t got = 0U;
      (void)ra_mipi_dsi_rx_payload_read(s_pending_rx_buffer, s_pending_rx_len, &got);
      s_pending_rx_buffer = nullptr;
      s_pending_rx_len    = 0U;
    }
  }
  /* HUM Ch 65.2 "RXRINFOOWSCR : Receive Result Info-Overwrite Clear", p 3879 */
  reg->RXRINFOOWSCR = (uint32_t)k_ra_mipi_dsi_rxrinfoow_sl0;
  internal_ra_mipi_dsi_call_user(k_ra_mipi_dsi_event_receive, bits);
}

void ra_mipi_dsi_dispatch_fatal(void)
{
  volatile r_mipi_dsi_regs_t* reg = ra_mipi_dsi();
  /* HUM Ch 65.2 "FERRSR : Fatal Error Status Register", p 3882 */
  const uint32_t bits = reg->FERRSR;
  /* HUM Ch 65.2 "FERRSCR : Fatal Error Status Clear", p 3884 */
  reg->FERRSCR = bits & (uint32_t)k_ra_mipi_dsi_ferrsr_clear_all;
  internal_ra_mipi_dsi_call_user(k_ra_mipi_dsi_event_fatal, bits);
}

void ra_mipi_dsi_dispatch_phy(void)
{
  volatile r_mipi_dsi_regs_t* reg = ra_mipi_dsi();
  /* HUM Ch 65.2 "PLSR : PHY Lane Status Register", p 3888 */
  const uint32_t bits = reg->PLSR;
  /* HUM Ch 65.2 "PLSCR : PHY Lane Status Clear", p 3889 */
  reg->PLSCR = bits & (uint32_t)k_ra_mipi_dsi_plsr_clear_all;
  internal_ra_mipi_dsi_call_user(k_ra_mipi_dsi_event_phy, bits);
}

void ra_mipi_dsi_dispatch(void)
{
  /* HUM Ch 65.2 "ISR : Interrupt Status Register", p 3840 */
  const uint32_t snapshot = ra_mipi_dsi()->ISR;
  if ((snapshot & (uint32_t)k_ra_mipi_dsi_isr_sq0) != 0U) {
    ra_mipi_dsi_dispatch_seq0();
  }
  if ((snapshot & (uint32_t)k_ra_mipi_dsi_isr_sq1) != 0U) {
    ra_mipi_dsi_dispatch_seq1();
  }
  if ((snapshot & (uint32_t)k_ra_mipi_dsi_isr_vm) != 0U) {
    ra_mipi_dsi_dispatch_video();
  }
  if ((snapshot & (uint32_t)k_ra_mipi_dsi_isr_rcv) != 0U) {
    ra_mipi_dsi_dispatch_receive();
  }
  if ((snapshot & (uint32_t)k_ra_mipi_dsi_isr_ferr) != 0U) {
    ra_mipi_dsi_dispatch_fatal();
  }
  if ((snapshot & (uint32_t)k_ra_mipi_dsi_isr_ppi) != 0U) {
    ra_mipi_dsi_dispatch_phy();
  }
  /* If nothing was set, still call user with mask=0 so the legacy
   * "always invoke" contract from the previous revision is preserved. */
  if ((snapshot & (uint32_t)k_ra_mipi_dsi_isr_all) == 0U) {
    internal_ra_mipi_dsi_call_user(k_ra_mipi_dsi_event_phy, 0U);
  }
}
