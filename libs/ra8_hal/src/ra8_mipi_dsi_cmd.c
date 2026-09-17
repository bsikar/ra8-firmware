/**
 * @file ra8_mipi_dsi_cmd.c
 * @brief MIPI DSI-2 sequence-channel command path (HUM Ch 65)
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Everything that puts a DCS/generic packet on the wire through a
 * sequence channel: descriptor construction, payload staging into the
 * FIFO, the SQCH start pulse, the LINKSR pre-condition checks, and the
 * short-packet / long-packet / read-packet entry points built on top of
 * `ra8_mipi_dsi_send_command()`.
 *
 * Split out of `ra8_mipi_dsi.c` along the seam the module already had.
 * The command path touches none of the driver's module state -- not
 * `s_initialized`, not the clock-mode or ULPS-lane flags -- so it needs
 * nothing from the lifecycle TU beyond `priv_ra8_mipi_dsi_internal_wait_eq()`,
 * which `ra8_mipi_dsi_internal.h` already publishes. The lifecycle,
 * software-reset, module-stop, HS-clock and ULPS paths stay in
 * `ra8_mipi_dsi.c`; the video-mode, status and IRQ-dispatch surfaces stay
 * in `ra8_mipi_dsi_dispatch.c`.
 *
 * The D-PHY analog block (HUM Ch 64) is owned by `ra8_mipi_phy.c`.
 *
 * Every register access carries a HUM Ch 65 citation in the form
 * required by `scripts/checks/cite_check.py`:
 *
 *   /\* HUM Ch 65.X "name", p NNNN *\/
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_log.h"
#include "ra8_mipi_dsi.h"
#include "ra8_mipi_dsi_internal.h"
#include "ra8_mipi_dsi_regs.h"

/**
 * @var s_tag
 * @brief Log tag for this translation unit.
 * @details Matches the tag used by the sibling MIPI DSI translation units
 *          so a console trace reads as one driver.
 * @note Read-only after initialisation.
 * @warning Do not modify at run time.
 * @since 0.1.0
 */
static const char* s_tag = "MIPI_DSI";

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
 * Command-path helpers
 * =============================================================================
 */

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
RA8_INTERNAL static uint32_t internal_ra8_mipi_dsi_make_dsc_a(const ra8_mipi_dsi_command_t* cmd)
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
RA8_INTERNAL static uint32_t internal_ra8_mipi_dsi_make_dsc_c(const ra8_mipi_dsi_command_t* cmd)
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
RA8_INTERNAL static void internal_ra8_mipi_dsi_stage_payload(const uint8_t* data, uint16_t len)
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
  /* HUM Ch 65.2 "TXPPD0R : Transmit Packet Payload Data 0", p 3849 */
  reg->TXPPD0R = words[0];
  /* HUM Ch 65.2 "TXPPD1R : Transmit Packet Payload Data 1", p 3850 */
  reg->TXPPD1R = words[1];
  /* HUM Ch 65.2 "TXPPD2R : Transmit Packet Payload Data 2", p 3850 */
  reg->TXPPD2R = words[2];
  /* HUM Ch 65.2 "TXPPD3R : Transmit Packet Payload Data 3", p 3851 */
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
RA8_INTERNAL static void internal_ra8_mipi_dsi_pulse_start(uint8_t channel)
{
  volatile r_mipi_dsi_regs_t* reg = ra8_mipi_dsi();
  const uint32_t              ch0_word =
    k_ra8_mipi_dsi_sqch_chsel | ((channel == 0U) ? k_ra8_mipi_dsi_sqch_start : 0U);
  const uint32_t ch1_word =
    k_ra8_mipi_dsi_sqch_chsel | ((channel == 1U) ? k_ra8_mipi_dsi_sqch_start : 0U);
  /* HUM Ch 65.2 "SQCH0SET0R : Sequence Channel 0 Setting 0", p 3899 */
  reg->SQCH0SET0R = ch0_word;
  /* HUM Ch 65.2 "SQCH1SET0R : Sequence Channel 1 Setting 0", p 3905 */
  reg->SQCH1SET0R = ch1_word;
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
RA8_INTERNAL static ra8_err_t internal_ra8_mipi_dsi_validate_cmd(const ra8_mipi_dsi_command_t* cmd)
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
RA8_INTERNAL static ra8_err_t internal_check_link_state(const ra8_mipi_dsi_command_t* cmd)
{
  volatile const r_mipi_dsi_regs_t* reg  = ra8_mipi_dsi();
  const uint32_t                    link = reg->LINKSR;
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
RA8_INTERNAL static void internal_program_descriptor(volatile r_mipi_dsi_descriptor_t* dsc,
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
RA8_INTERNAL static void internal_send_stage_and_pulse(const ra8_mipi_dsi_command_t* cmd)
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
  RA8_RETURN_ON_ERROR(v_err, s_tag, "send_command: validate");

  const ra8_err_t link_err = internal_check_link_state(cmd);
  RA8_RETURN_ON_ERROR(link_err, s_tag, "send_command: link state");

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
