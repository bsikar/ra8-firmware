/**
 * @file ra8_i2c_config.c
 * @brief I2C Bus Interface (IIC) bring-up plane
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Configuration-plane half of the RA8D2 RIIC polling driver, split out of
 * ``ra8_i2c.c`` to keep each translation unit under the file-size cap. Owns
 * the init / deinit sequence (HUM Ch 39.3.2 "Initial Settings" p 2395).
 * The bit-rate solver and ``ra8_i2c_set_clock`` moved to
 * ``i2c_clock_abi.zig`` (RA8FW-695). The
 * error-status helpers (``ra8_i2c_get_errors`` / ``ra8_i2c_clear_errors``)
 * moved to ``i2c_status_abi.zig`` (RA8FW-694).
 *
 * The data-transfer plane (start / write / read / stop / scan) lives in
 * ``ra8_i2c.c``. Both translation units share ``s_i2c_state`` and the log
 * tag via ``ra8_i2c_internal.h``.
 *
 * Owns every write to the RIIC register block performed during channel
 * bring-up and clock setup. See HUM Ch 39 "I2C Bus Interface (IIC)",
 * p 2367-2470.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_i2c.h"
#include "ra8_i2c_internal.h"
#include "ra8_i2c_regs.h"
#include "ra8_log.h"
#include "ra8_mstp.h"

/**
 * @brief Map a channel index to its MSTP gate id.
 *
 * @details
 * IIC0 = MSTPB9, IIC1 = MSTPB8, IIC2 = MSTPB7 per HUM Ch 11.2.7
 * "MSTPCRB" p 444, encoded in ``ra8_mstp_regs.h``.
 *
 * @param[in] channel Channel index (already range-checked by caller).
 * @return The matching ``k_ra8_mstp_iicN`` enum value.
 * @retval k_ra8_mstp_iic0 ``channel`` is 0.
 * @retval k_ra8_mstp_iic1 ``channel`` is 1.
 * @retval k_ra8_mstp_iic2 ``channel`` is 2 (or any other value, defensively).
 *
 * @pre ``channel`` is 0, 1 or 2.
 * @pre Caller resolved a non-NULL register pointer for ``channel``.
 * @post Return value is one of the three IIC MSTP ids.
 * @post No global state is mutated.
 * @note Thread safety: pure mapping, no state.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_mstp_t internal_i2c_mstp_id(uint8_t channel)
{
  if (channel == 0U) {
    return k_ra8_mstp_iic0;
  }
  if (channel == 1U) {
    return k_ra8_mstp_iic1;
  }
  return k_ra8_mstp_iic2;
}

/* =============================================================================
 * Init / deinit -- mirrors HUM Ch 39.3.2 "Initial Settings" p 2395.
 * =============================================================================
 */

/**
 * @brief Apply the bring-up register sequence for an IIC channel.
 *
 * @details
 * Follows HUM Ch 39.3.2 p 2395: hold IIC reset (ICCR1.IICRST with
 * ICE = 0), enable internal reset (ICE = 1), program CKS / ICBRL /
 * ICBRH and the ICFER function bits, then release the reset. FMPE is
 * set for the Fast-mode Plus (>= 1 MHz) bus rate.
 *
 * @param[in] reg Channel register block.
 * @param[in] cks CKS divider exponent.
 * @param[in] brh ICBRH register value.
 * @param[in] brl ICBRL register value.
 * @param[in] fast_plus True when the bus runs at Fast-mode Plus.
 *
 * @pre reg is non-NULL.
 * @pre Channel MSTP gate already ungated.
 * @post ICCR1.ICE is set and the channel is out of reset.
 * @post ICMR1.CKS, ICBRL, ICBRH and ICFER hold the programmed values.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_i2c_apply_init_regs(volatile r_i2c_regs_t* reg,
                                                      uint8_t                cks,
                                                      uint8_t                brh,
                                                      uint8_t                brl,
                                                      bool                   fast_plus)
{
  /* Initial-settings step 1 (Figure 39.5): ICE = 0, pins inactive.
   * HUM Ch 39.2.1 "ICCR1 : I2C Bus Control Register 1" p 2369 */
  reg->ICCR1 = 0U;
  /* Initial-settings step 2: IICRST = 1 (IIC reset, ICE still 0).
   * HUM Ch 39.2.1 "ICCR1 : I2C Bus Control Register 1" p 2369 */
  reg->ICCR1 = (uint8_t)k_ra8_i2c_msk_iccr1_iicrst;
  /* Initial-settings step 3: ICE = 1 (internal reset, pins active).
   * HUM Ch 39.2.1 "ICCR1 : I2C Bus Control Register 1" p 2369 */
  reg->ICCR1 = (uint8_t)((uint8_t)k_ra8_i2c_msk_iccr1_iicrst | (uint8_t)k_ra8_i2c_msk_iccr1_ice);

  /* HUM Ch 39.2.3 "ICMR1 : I2C Bus Mode Register 1 -- CKS[6:4]" p 2374 */
  reg->ICMR1 = (uint8_t)((uint32_t)cks << (uint32_t)k_ra8_i2c_icmr1_cks_pos);
  /* HUM Ch 39.2.15 "ICBRL : I2C Bus Bit Rate Low-Level Register" p 2391 */
  reg->ICBRL = brl;
  /* HUM Ch 39.2.16 "ICBRH : I2C Bus Bit Rate High-Level Register" p 2392 */
  reg->ICBRH = brh;

  /* Enable arbitration-lost detection, NACK transfer suspension, the SCL
   * synchronous circuit, and (for Fm+) the slope-control circuit.
   * HUM Ch 39.2.6 "ICFER : I2C Bus Function Enable Register" p 2378 */
  uint8_t icfer = (uint8_t)((uint8_t)k_ra8_i2c_msk_icfer_male | (uint8_t)k_ra8_i2c_msk_icfer_nacke |
                            (uint8_t)k_ra8_i2c_msk_icfer_scle);
  if (fast_plus) {
    icfer |= (uint8_t)k_ra8_i2c_msk_icfer_fmpe;
  }
  reg->ICFER = icfer;

  /* Initial-settings step 5: release the internal reset (IICRST = 0).
   * HUM Ch 39.2.1 "ICCR1 : I2C Bus Control Register 1" p 2369 */
  reg->ICCR1 = (uint8_t)k_ra8_i2c_msk_iccr1_ice;
}

ra8_err_t ra8_i2c_init(uint8_t channel, const ra8_i2c_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, g_i2c_tag, "i2c_init: cfg");
  uint8_t         cks    = 0U;
  uint8_t         brh    = 0U;
  uint8_t         brl    = 0U;
  const ra8_err_t br_err = priv_ra8_i2c_internal_bitrate(cfg->bus_hz, cfg->pclkb_hz, &cks, &brh, &brl);
  RA8_RETURN_ON_ERROR(br_err, g_i2c_tag, "i2c_init: bitrate");

  volatile r_i2c_regs_t* reg = ra8_i2c_regs(channel);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }

  const ra8_err_t mst_err = ra8_mstp_enable(internal_i2c_mstp_id(channel));
  RA8_RETURN_ON_ERROR(mst_err, g_i2c_tag, "i2c_init: mstp");

  internal_i2c_apply_init_regs(reg,
                               cks,
                               brh,
                               brl,
                               cfg->bus_hz >= (uint32_t)k_ra8_i2c_speed_fast_plus);

  s_i2c_state[channel].initialized = true;
  s_i2c_state[channel].bus_held    = false;

  ra8_log_info_val(g_i2c_tag, "i2c_init channel", (uint32_t)channel);
  return k_ra8_ok;
}

ra8_err_t ra8_i2c_deinit(uint8_t channel)
{
  volatile r_i2c_regs_t* reg = ra8_i2c_regs(channel);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  /* ICE = 0: place the SCL/SDA pins back in the inactive state.
   * HUM Ch 39.2.1 "ICCR1 : I2C Bus Control Register 1" p 2369 */
  reg->ICCR1                       = 0U;
  s_i2c_state[channel].initialized = false;
  s_i2c_state[channel].bus_held    = false;
  return ra8_mstp_disable(internal_i2c_mstp_id(channel));
}
