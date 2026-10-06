/**
 * @file ra8_i3c.c
 * @brief I3C Bus Interface driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Primary-mode driver for the RA8D2 I3C0 controller.  The transfer
 * engine mirrors FSP ``r_i3c``: every operation is encoded as a
 * 32-bit (or two-word) command descriptor written to the NCMDQP
 * port, with payload bytes flowing through NTDTBP0.  IBI events
 * arrive via the NIBIQP queue.
 *
 * Bring-up order matches the FSP ``R_I3C_Open`` reference sequence:
 * enable the module clock (CECTL.CLKE), drop BCTL.BUSE, assert
 * RSTCTL.RI3CRST and wait for hardware to clear it, then assert
 * RSTCTL.INTLRST, clear PRTS, release RSTCTL.  Primary dynamic
 * address (MSDVAD.MDYAD) is programmed before BCTL.BUSE is set per
 * HUM Ch 40 BCTL description (pp 2445-2701).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_i3c.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_i3c_i2c.h"
#include "ra8_i3c_i2c_peripheral.h"
#include "ra8_i3c_i2c_regs.h"
#include "ra8_i3c_internal.h"
#include "ra8_i3c_regs.h"
#include "ra8_log.h"
#include "ra8_mstp.h"
#include "ra8_mstp_regs.h"

/**
 * @brief Pure recv-ccc-invalid predicate -- see header for full contract.
 * @details Promoted helper so the line-688 OR can be driven under MC/DC.
 * @param[in] addr_mask Maximum valid 7-bit address mask.
 * @param[in] target    Candidate target address.
 * @param[in] max_len   Caller-supplied max read length.
 * @return Boolean reject predicate.
 * @retval true  Inputs invalid.
 * @retval false Inputs OK.
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return depends solely on inputs.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
bool priv_ra8_i3c_internal_recv_ccc_invalid(uint8_t addr_mask, uint8_t target, uint8_t max_len)
{
  return (target > addr_mask) || (max_len == 0U);
}

/**
 * @brief Pure HDR-mode-invalid predicate -- see header for full contract.
 * @details Promoted helper so the line-815 AND can be driven under MC/DC.
 * @param[in] sdr_val Numeric value of @c k_ra8_i3c_hdr_mode_sdr.
 * @param[in] ddr_val Numeric value of @c k_ra8_i3c_hdr_mode_ddr.
 * @param[in] ts_val  Numeric value of @c k_ra8_i3c_hdr_mode_ts.
 * @param[in] mode    Candidate mode value.
 * @return Boolean reject predicate.
 * @retval true  Mode is not legal.
 * @retval false Mode is legal.
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return depends solely on inputs.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
bool priv_ra8_i3c_internal_hdr_mode_invalid(uint32_t sdr_val,
                                            uint32_t ddr_val,
                                            uint32_t ts_val,
                                            uint32_t mode)
{
  return (mode != sdr_val) && (mode != ddr_val) && (mode != ts_val);
}

/** @brief Log tag for this driver. */
static const char* const s_tag = "I3C";

/* =============================================================================
 * Private helpers
 * =============================================================================
 */

RA8_INTERNAL static uint32_t internal_ra8_i3c_xfer_cmd_word(uint8_t target_addr, bool rnw)
{
  uint32_t cmd = 0U;
  cmd |= ((uint32_t)target_addr) << k_ra8_i3c_cmd_dev_index_shift;
  if (rnw) {
    cmd |= 1U << k_ra8_i3c_cmd_rnw_shift;
  }
  cmd |= 1U << k_ra8_i3c_cmd_roc_shift; /* response on completion         */
  cmd |= 1U << k_ra8_i3c_cmd_toc_shift; /* terminate (STOP) on completion */
  return cmd;
}

RA8_INTERNAL static void
internal_ra8_i3c_fifo_read(volatile const r_i3c_regs_t* reg, uint8_t* out, uint32_t len)
{
  uint32_t       i           = 0U;
  const uint32_t k_word_size = k_ra8_i3c_word_size;
  while (i + k_word_size <= len) {
    const uint32_t w = reg->NTDTBP0;
    out[i]           = (uint8_t)(w & k_ra8_i3c_byte_mask);
    out[i + 1U]      = (uint8_t)((w >> k_ra8_i3c_shift_b1) & k_ra8_i3c_byte_mask);
    out[i + 2U]      = (uint8_t)((w >> k_ra8_i3c_shift_b2) & k_ra8_i3c_byte_mask);
    out[i + 3U]      = (uint8_t)((w >> k_ra8_i3c_shift_b3) & k_ra8_i3c_byte_mask);
    i += k_word_size;
  }
  if (i < len) {
    const uint32_t w = reg->NTDTBP0;
    uint32_t       s = 0U;
    while (i < len) {
      out[i] = (uint8_t)((w >> s) & k_ra8_i3c_byte_mask);
      s += k_ra8_i3c_byte_shift;
      ++i;
    }
  }
}

/* =============================================================================
 * I2C-compatibility mode (delegates to the legacy IIC_B path)
 * =============================================================================
 */

ra8_err_t ra8_i3c_set_clock(uint8_t channel, uint32_t bus_hz, uint32_t pclka_hz)
{
  if ((uint16_t)channel >= (uint16_t)k_ra8_i3c_i2c_channel_count) {
    return k_ra8_err_invalid_arg;
  }
  if (s_i3c_chan[channel].mode != k_ra8_i3c_mode_i2c) {
    return k_ra8_err_invalid_state;
  }
  return ra8_i3c_i2c_set_clock(channel, bus_hz, pclka_hz);
}

ra8_err_t ra8_i3c_scan(uint8_t channel, uint8_t addr, bool* out_acked)
{
  if ((uint16_t)channel >= (uint16_t)k_ra8_i3c_i2c_channel_count) {
    return k_ra8_err_invalid_arg;
  }
  if (s_i3c_chan[channel].mode != k_ra8_i3c_mode_i2c) {
    return k_ra8_err_invalid_state;
  }
  return ra8_i3c_i2c_scan(channel, addr, out_acked);
}

ra8_err_t ra8_i3c_get_errors(uint8_t channel, uint8_t* out_mask)
{
  if ((uint16_t)channel >= (uint16_t)k_ra8_i3c_i2c_channel_count) {
    return k_ra8_err_invalid_arg;
  }
  if (s_i3c_chan[channel].mode != k_ra8_i3c_mode_i2c) {
    return k_ra8_err_invalid_state;
  }
  return ra8_i3c_i2c_get_errors(channel, out_mask);
}

ra8_err_t ra8_i3c_clear_errors(uint8_t channel)
{
  if ((uint16_t)channel >= (uint16_t)k_ra8_i3c_i2c_channel_count) {
    return k_ra8_err_invalid_arg;
  }
  if (s_i3c_chan[channel].mode != k_ra8_i3c_mode_i2c) {
    return k_ra8_err_invalid_state;
  }
  return ra8_i3c_i2c_clear_errors(channel);
}

ra8_err_t ra8_i3c_abort(uint8_t channel)
{
  if ((uint16_t)channel >= (uint16_t)k_ra8_i3c_i2c_channel_count) {
    return k_ra8_err_invalid_arg;
  }
  if (s_i3c_chan[channel].mode != k_ra8_i3c_mode_i2c) {
    return k_ra8_err_invalid_state;
  }
  return ra8_i3c_i2c_abort(channel);
}

/* =============================================================================
 * I2C-compatibility peripheral (responder) mode
 * =============================================================================
 */

ra8_err_t ra8_i3c_peripheral_open(uint8_t channel, const ra8_i3c_peripheral_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "peripheral_open: cfg");
  if ((uint16_t)channel >= (uint16_t)k_ra8_i3c_i2c_channel_count) {
    return k_ra8_err_invalid_arg;
  }
  /* Responder open is a self-contained bring-up; mark the channel I2C so
   * the close/send/receive/status guards accept it. */
  const ra8_i3c_i2c_peripheral_cfg_t bcfg = {.peripheral_addr_7b = cfg->peripheral_addr_7b,
                                             .general_call       = cfg->general_call};
  const ra8_err_t                    err  = ra8_i3c_i2c_peripheral_open(channel, &bcfg);
  if (err == k_ra8_ok) {
    s_i3c_chan[channel].mode        = k_ra8_i3c_mode_i2c;
    s_i3c_chan[channel].initialized = true;
  }
  return err;
}

ra8_err_t ra8_i3c_peripheral_close(uint8_t channel)
{
  if ((uint16_t)channel >= (uint16_t)k_ra8_i3c_i2c_channel_count) {
    return k_ra8_err_invalid_arg;
  }
  if (s_i3c_chan[channel].mode != k_ra8_i3c_mode_i2c) {
    return k_ra8_err_invalid_state;
  }
  return ra8_i3c_i2c_peripheral_close(channel);
}

ra8_err_t ra8_i3c_peripheral_send(uint8_t channel, const uint8_t* data, uint32_t len)
{
  if ((uint16_t)channel >= (uint16_t)k_ra8_i3c_i2c_channel_count) {
    return k_ra8_err_invalid_arg;
  }
  if (s_i3c_chan[channel].mode != k_ra8_i3c_mode_i2c) {
    return k_ra8_err_invalid_state;
  }
  return ra8_i3c_i2c_peripheral_send(channel, data, len);
}

ra8_err_t ra8_i3c_peripheral_receive(uint8_t channel, uint8_t* buf, uint32_t len)
{
  if ((uint16_t)channel >= (uint16_t)k_ra8_i3c_i2c_channel_count) {
    return k_ra8_err_invalid_arg;
  }
  if (s_i3c_chan[channel].mode != k_ra8_i3c_mode_i2c) {
    return k_ra8_err_invalid_state;
  }
  return ra8_i3c_i2c_peripheral_receive(channel, buf, len);
}

ra8_err_t ra8_i3c_peripheral_status(uint8_t channel, uint8_t* out_mask)
{
  if ((uint16_t)channel >= (uint16_t)k_ra8_i3c_i2c_channel_count) {
    return k_ra8_err_invalid_arg;
  }
  if (s_i3c_chan[channel].mode != k_ra8_i3c_mode_i2c) {
    return k_ra8_err_invalid_state;
  }
  return ra8_i3c_i2c_peripheral_status(channel, out_mask);
}

/* =============================================================================
 * Sweep 15 / Phase 2: HDR mode + IBI control + peripheral-mode entry
 * =============================================================================
 */

ra8_err_t ra8_i3c_set_hdr_mode(uint8_t target_addr, ra8_i3c_hdr_mode_t mode)
{
  if (target_addr > (uint8_t)k_ra8_i3c_addr_mask) {
    return k_ra8_err_invalid_arg;
  }
  if (priv_ra8_i3c_internal_hdr_mode_invalid((uint32_t)k_ra8_i3c_hdr_mode_sdr,
                                             (uint32_t)k_ra8_i3c_hdr_mode_ddr,
                                             (uint32_t)k_ra8_i3c_hdr_mode_ts,
                                             (uint32_t)mode)) {
    return k_ra8_err_invalid_arg;
  }

  /* HUM Ch 40 "Command Descriptor / Transfer Mode" pp 2445-2701 --
   * regular-xfer attribute (0) + dynamic address + transfer-mode
   * bits at [27:26]. */
  uint32_t       cmd       = internal_ra8_i3c_xfer_cmd_word(target_addr, false);
  const uint32_t mode_bits = ((uint32_t)mode & k_ra8_i3c_cmd_hdr_mode_mask)
                             << k_ra8_i3c_cmd_xfer_mode_shift;
  cmd |= mode_bits;

  volatile r_i3c_regs_t* reg = ra8_i3c();
  reg->NCMDQP                = cmd;
  reg->NTST                  = reg->NTST & ~k_ra8_i3c_ntst_cmdqef_mask;
  return k_ra8_ok;
}

ra8_err_t ra8_i3c_ibi_enable(uint8_t target_addr)
{
  if (target_addr > (uint8_t)k_ra8_i3c_addr_mask) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 40 "IBI Valid Control Register" pp 2445-2701 -- VLCNT[7:0]
   * carries the count of IBI entries the controller will accept. */
  enum : uint32_t {
    k_ra8_i3c_ntibivctl_one_target = 1U, /**< RA8 I3C ntibivctl one target. */
  };
  volatile r_i3c_regs_t* reg = ra8_i3c();
  reg->NTIBIVCTL             = (k_ra8_i3c_ntibivctl_one_target << k_ra8_i3c_ntibivctl_vlcnt_shift) &
                               k_ra8_i3c_ntibivctl_vlcnt_mask;
  (void)target_addr;
  return k_ra8_ok;
}

ra8_err_t ra8_i3c_target_open(uint8_t static_addr)
{
  if (static_addr > (uint8_t)k_ra8_i3c_addr_mask) {
    return k_ra8_err_invalid_arg;
  }

  volatile r_i3c_regs_t* reg = ra8_i3c();
  /* HUM Ch 40 "BCTL : Bus Control Register" pp 2445-2701 -- drop BUSE
   * before flipping the SLVE bit. */
  reg->BCTL = reg->BCTL & ~k_ra8_i3c_bctl_buse_mask;

  /* HUM Ch 40 "NSDVAD : Peripheral Device Address Register" p 2445-2701 */
  const uint32_t sdyad =
    (((uint32_t)static_addr) << k_ra8_i3c_nsdvad_sdyad_shift) & k_ra8_i3c_nsdvad_sdyad_mask;
  reg->NSDVAD = sdyad | k_ra8_i3c_nsdvad_sdyadv_mask;

  /* HUM Ch 40 "BCTL : Bus Control Register" pp 2445-2701 -- bit 16
   * (SLVE) gates peripheral-mode reception. */
  reg->BCTL = reg->BCTL | k_ra8_i3c_bctl_slve_mask;
  return k_ra8_ok;
}

/* =============================================================================
 * IBI inbound queue
 * =============================================================================
 */

ra8_err_t ra8_i3c_ibi_read(ra8_i3c_ibi_t* out_ibi)
{
  RA8_CHECK_NULL_PTR(out_ibi, s_tag, "out_ibi must not be nullptr");

  volatile r_i3c_regs_t* reg = ra8_i3c();
  /* NTST.IBIQEFF (bit 2) is set when the IBI queue holds at least
   * one entry.  See HUM Ch 40 "NTST" pp 2445-2701. */
  if ((reg->NTST & k_ra8_i3c_ntst_ibiqeff_mask) == 0U) {
    return k_ra8_err_no_data;
  }

  const uint32_t status = reg->NIBIQP;
  const uint8_t  len =
    (uint8_t)((status & k_ra8_i3c_ibi_status_length_mask) >> k_ra8_i3c_ibi_status_length_shift);
  const uint8_t ibi_id =
    (uint8_t)((status & k_ra8_i3c_ibi_status_id_mask) >> k_ra8_i3c_ibi_status_id_shift);

  out_ibi->address = (uint8_t)(ibi_id >> k_ra8_i3c_addr_shift_in_ibi_id);
  /* Encode IBI/HJ/MR using the lowest two bits of IBI ID.  See HUM
   * Ch 40 "IBI Status Descriptor" pp 2445-2701 -- bit 31 (IBI_ST)
   * separates IBI from HJ/MR; IBI_ID 0x02 == hot-join, 0x04 ==
   * mainship-request in the MIPI I3C spec. */
  enum : uint8_t {
    k_ibi_id_hot_join = 0x02U, /**< Ibi ID hot join. */
  };
  if ((status & k_ra8_i3c_ibi_status_ibi_st_mask) == 0U) {
    out_ibi->type = k_ra8_i3c_ibi_type_interrupt;
  } else if (ibi_id == k_ibi_id_hot_join) {
    out_ibi->type = k_ra8_i3c_ibi_type_hot_join;
  } else {
    out_ibi->type = k_ra8_i3c_ibi_type_main_request;
  }

  const uint8_t k_max_ibi_payload = (uint8_t)(sizeof(out_ibi->payload));
  uint8_t       to_copy           = len;
  if (to_copy > k_max_ibi_payload) {
    to_copy = k_max_ibi_payload;
  }
  out_ibi->payload_len = to_copy;
  for (uint8_t i = 0U; i < k_max_ibi_payload; ++i) {
    out_ibi->payload[i] = 0U;
  }
  if (to_copy > 0U) {
    internal_ra8_i3c_fifo_read(reg, out_ibi->payload, (uint32_t)to_copy);
  }
  enum : uint32_t {
    k_ibi_last_mask = 1U, /**< Ibi last mask. */
  };
  out_ibi->last = (uint8_t)((status >> k_ra8_i3c_ibi_status_last_shift) & k_ibi_last_mask);

  /* Clear IBIQEFF so the next read can detect a fresh entry. */
  reg->NTST = reg->NTST & ~k_ra8_i3c_ntst_ibiqeff_mask;
  return k_ra8_ok;
}
