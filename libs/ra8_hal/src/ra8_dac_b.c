/**
 * @file ra8_dac_b.c
 * @brief 12-Bit D/A Converter (DAC_B) driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Driver for the RA8D2 DAC_B peripheral. DAC_B is the RA8 successor
 * to the pre-RA8 DAC12 block: each DAC "channel" is a separate IP
 * instance with its own DADR + DACR0/1/2 set. DAC_B0 and DAC_B1
 * live at 0x40233000 and 0x40233100 (stride 0x100). The driver
 * hides this by keeping a 2-channel public API, with channel 0
 * routed to DAC_B0 and channel 1 routed to DAC_B1.
 *
 * Cross-verified against FSP r_dac_b.c (R_DAC_B_Open / Write /
 * Start / Stop / Close paths). The bit layout used here mirrors
 * FSP `R_DAC_B0_DACR{0,1,2}_*_Msk` exactly. See HUM Ch 54 "12-Bit
 * D/A Converter (DAC12)" p 3490..3496.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_dac_b.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_dac_b_regs.h"
#include "ra8_err.h"
#include "ra8_log.h"
#include "ra8_mstp.h"

static const char* s_tag = "DAC_B";

/**
 * @enum ra8_dac_b_channel_t
 * @brief Named channel indices.
 */
typedef enum : uint8_t {
  k_ra8_dac_b_channel_0 = 0U, /**< RA8 DAC b channel 0. */
  k_ra8_dac_b_channel_1 = 1U, /**< RA8 DAC b channel 1. */
} ra8_dac_b_channel_t;

/**
 * @brief Clamp a raw value to 12-bit range.
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @param[in] value See header declaration for direction and constraints.
 * @return ``ra8_err_t`` error code (or void if the signature returns void).
 * @retval k_ra8_ok Success path.
 * @retval k_ra8_err_invalid_arg Caller violated a precondition.
 * @pre Driver state has been initialized by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
RA8_INTERNAL static inline uint16_t internal_ra8_dac_b_clamp(uint16_t value)
{
  if ((uint32_t)value > (uint32_t)k_ra8_dac_b_max_value) {
    return k_ra8_dac_b_max_value;
  }
  return value;
}

/**
 * @brief Stop a channel and put DACR0 in the FSP "open" baseline.
 *
 * @details
 * Mirrors `p_ctrl->p_reg->DACR0 &= ~(DACEN | DAOUTDIS | DAE)` from
 * FSP R_DAC_B_Open. Because we always come in here from a fresh
 * power-on (MSTP just released), an unconditional zero is
 * equivalent and avoids a read-modify-write hazard on a
 * just-clocked register block.
 *
 * Citation: HUM Ch 54 "12-Bit D/A Converter (DAC12)" p 3490.
 *
 * @param[in] channel See header declaration for direction and constraints.
 * @pre Driver state has been initialized by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_disable_channel(uint8_t channel)
{
  volatile r_dac_b_regs_t* reg = ra8_dac_b(channel);
  if (reg == nullptr) {
    return;
  }
  /* HUM Ch 54 "12-Bit D/A Converter (DAC12)" p 3490 */
  reg->DACR0 = 0U;
  reg->DADR  = 0U;
}

/**
 * @brief Set DACEN=1 on a channel (FSP R_DAC_B_Start equivalent).
 *
 * @details
 * Pure read-modify-write of bit 0 of DACR0; preserves DAE and
 * DAOUTDIS so callers can configure those in DACR0 *before*
 * starting conversion.
 *
 * @param[in] channel See header declaration for direction and constraints.
 * @pre Driver state has been initialized by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_start_channel(uint8_t channel)
{
  volatile r_dac_b_regs_t* reg = ra8_dac_b(channel);
  if (reg == nullptr) {
    return;
  }
  /* HUM Ch 54 "12-Bit D/A Converter (DAC12)" p 3490 */
  reg->DACR0 = reg->DACR0 | k_ra8_dacr0_mask_dacen;
}

/**
 * @brief Clear DACEN on a channel (FSP R_DAC_B_Stop equivalent).
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @param[in] channel See header declaration for direction and constraints.
 * @pre Driver state has been initialized by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_stop_channel(uint8_t channel)
{
  volatile r_dac_b_regs_t* reg = ra8_dac_b(channel);
  if (reg == nullptr) {
    return;
  }
  /* HUM Ch 54 "12-Bit D/A Converter (DAC12)" p 3490 */
  reg->DACR0 = reg->DACR0 & ~k_ra8_dacr0_mask_dacen;
}

[[nodiscard]] ra8_err_t ra8_dac_b_init(void)
{
  /* DAC_B0 and DAC_B1 have separate MSTP bits.
   * HUM Ch 11.2.9 "MSTPCRD : Module Stop Control Register D", p 448 */
  ra8_err_t mst_err = ra8_mstp_enable(k_ra8_mstp_dac12_0);
  /* GCOVR_EXCL_BR_START -- MSTP HW readback */
  RA8_RETURN_ON_ERROR(mst_err, s_tag, "dac_b_init: mstp dac0");
  /* GCOVR_EXCL_BR_STOP */
  mst_err = ra8_mstp_enable(k_ra8_mstp_dac12_1);
  /* GCOVR_EXCL_BR_START -- MSTP HW readback */
  RA8_RETURN_ON_ERROR(mst_err, s_tag, "dac_b_init: mstp dac1");
  /* GCOVR_EXCL_BR_STOP */

  internal_disable_channel(k_ra8_dac_b_channel_0);
  internal_disable_channel(k_ra8_dac_b_channel_1);
  ra8_log_info(s_tag, "dac_b_init");
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_dac_b_write(uint8_t channel, uint16_t value)
{
  volatile r_dac_b_regs_t* reg = ra8_dac_b(channel);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  const uint16_t clamped = internal_ra8_dac_b_clamp(value);
  /* HUM Ch 54 "12-Bit D/A Converter (DAC12)" p 3490 -- FSP R_DAC_B_Write
   * is a single 16-bit store to DADR; DACEN is asserted via
   * ra8_dac_b_set_output_enable() / ra8_dac_b_init_configured(), NOT here. */
  reg->DADR = clamped;
  ra8_log_info_val(s_tag, "dac_b_write value", (uint32_t)clamped);
  return k_ra8_ok;
}

/**
 * @struct ra8_dac_b_state_t
 * @brief Driver-wide runtime state.
 */
typedef struct {
  ra8_dac_b_update_fn_t fn;  /**< Fn.  */
  void*                 ctx; /**< Ctx. */
} ra8_dac_b_state_t;

static ra8_dac_b_state_t s_dac_b_state;

/**
 * @brief Apply ``cfg`` to one DAC_B instance (FSP R_DAC_B_Open body).
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 * @param[in] channel See header declaration for direction and constraints.
 * @param[in] cfg See header declaration for direction and constraints.
 * @pre Driver state has been initialized by the matching ``*_init``.
 * @pre Caller has validated all pointer parameters.
 * @post Side effects are limited to those documented in the header.
 * @post No global state is modified on the error path.
 * @note Thread safety: see the header declaration.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_apply_cfg(uint8_t channel, const ra8_dac_b_cfg_t* cfg)
{
  volatile r_dac_b_regs_t* reg = ra8_dac_b(channel);
  if (reg == nullptr) {
    return;
  }
  /* HUM Ch 54 "12-Bit D/A Converter (DAC12)" p 3490..3496 -- mirror
   * FSP R_DAC_B_Open: clear DACR0, set DACR1.DPSEL, DACR2.OFSSEL,
   * zero DADR, then drive the DAOUTDIS bit per the descriptor. */
  reg->DACR0 = 0U;
  reg->DACR1 = ((uint32_t)cfg->data_format) << (uint32_t)k_ra8_dacr1_bit_dpsel;
  reg->DACR2 = ((uint32_t)cfg->vref) << (uint32_t)k_ra8_dacr2_bit_ofssel;
  reg->DADR  = 0U;
  /* internal_output_enabled == true clears DAOUTDIS (route to pin);
   * false sets DAOUTDIS (Hi-Z). FSP encodes the same way: it writes
   * the boolean directly into the 1-bit DAOUTDIS field. */
  if (!cfg->internal_output_enabled) {
    reg->DACR0 = reg->DACR0 | k_ra8_dacr0_mask_daoutdis;
  }
}

ra8_err_t ra8_dac_b_init_configured(const ra8_dac_b_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");

  ra8_err_t mst_err = ra8_mstp_enable(k_ra8_mstp_dac12_0);
  /* GCOVR_EXCL_BR_START -- MSTP HW readback */
  RA8_RETURN_ON_ERROR(mst_err, s_tag, "dac_b_init_cfg: mstp dac0");
  /* GCOVR_EXCL_BR_STOP */
  mst_err = ra8_mstp_enable(k_ra8_mstp_dac12_1);
  /* GCOVR_EXCL_BR_START -- MSTP HW readback */
  RA8_RETURN_ON_ERROR(mst_err, s_tag, "dac_b_init_cfg: mstp dac1");
  /* GCOVR_EXCL_BR_STOP */

  internal_apply_cfg(k_ra8_dac_b_channel_0, cfg);
  internal_apply_cfg(k_ra8_dac_b_channel_1, cfg);

  if (cfg->enable_channel0) {
    internal_start_channel(k_ra8_dac_b_channel_0);
  }
  if (cfg->enable_channel1) {
    internal_start_channel(k_ra8_dac_b_channel_1);
  }
  ra8_log_info(s_tag, "dac_b_init_configured");
  return k_ra8_ok;
}

ra8_err_t ra8_dac_b_deinit(void)
{
  /* HUM Ch 54 "12-Bit D/A Converter (DAC12)" p 3490 -- FSP R_DAC_B_Close
   * writes DACR0 = DAOUTDIS_Msk to fully disable output, operation,
   * and coupled operation. We then release MSTP. */
  volatile r_dac_b_regs_t* reg0 = ra8_dac_b(k_ra8_dac_b_channel_0);
  volatile r_dac_b_regs_t* reg1 = ra8_dac_b(k_ra8_dac_b_channel_1);
  if (reg0 != nullptr) {
    reg0->DACR0 = k_ra8_dacr0_mask_daoutdis;
    reg0->DADR  = 0U;
  }
  if (reg1 != nullptr) {
    reg1->DACR0 = k_ra8_dacr0_mask_daoutdis;
    reg1->DADR  = 0U;
  }
  s_dac_b_state.fn  = nullptr;
  s_dac_b_state.ctx = nullptr;
  (void)ra8_mstp_disable(k_ra8_mstp_dac12_1);
  return ra8_mstp_disable(k_ra8_mstp_dac12_0);
}

ra8_err_t ra8_dac_b_set_vref(ra8_dac_b_vref_t vref)
{
  /* HUM Ch 54 "12-Bit D/A Converter (DAC12)" p 3490 -- DACR2.OFSSEL is
   * a single bit at position 8; preserve all other bits. */
  volatile r_dac_b_regs_t* reg0    = ra8_dac_b(k_ra8_dac_b_channel_0);
  volatile r_dac_b_regs_t* reg1    = ra8_dac_b(k_ra8_dac_b_channel_1);
  const uint32_t           shifted = ((uint32_t)vref) << (uint32_t)k_ra8_dacr2_bit_ofssel;
  reg0->DACR2 = (reg0->DACR2 & ~k_ra8_dacr2_mask_ofssel) | (shifted & k_ra8_dacr2_mask_ofssel);
  reg1->DACR2 = (reg1->DACR2 & ~k_ra8_dacr2_mask_ofssel) | (shifted & k_ra8_dacr2_mask_ofssel);
  return k_ra8_ok;
}

ra8_err_t ra8_dac_b_set_output_enable(uint8_t channel, bool enable)
{
  volatile r_dac_b_regs_t* reg = ra8_dac_b(channel);
  if (reg == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 54 "12-Bit D/A Converter (DAC12)" p 3490 -- FSP toggles
   * DACEN (bit 0) only; DAE (batch mode) and DAOUTDIS are managed
   * separately. */
  if (enable) {
    reg->DACR0 = reg->DACR0 | k_ra8_dacr0_mask_dacen;
  } else {
    reg->DACR0 = reg->DACR0 & ~k_ra8_dacr0_mask_dacen;
  }
  return k_ra8_ok;
}

ra8_err_t ra8_dac_b_get_status(uint8_t* out_mask)
{
  RA8_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  /* Build a single-byte composite status flag:
   * bit 0 -- channel 0 DACEN
   * bit 1 -- channel 1 DACEN
   */
  uint8_t                        flags = 0U;
  volatile const r_dac_b_regs_t* reg0  = ra8_dac_b(k_ra8_dac_b_channel_0);
  volatile const r_dac_b_regs_t* reg1  = ra8_dac_b(k_ra8_dac_b_channel_1);
  if ((reg0->DACR0 & k_ra8_dacr0_mask_dacen) != 0U) {
    flags |= 0x1U;
  }
  if ((reg1->DACR0 & k_ra8_dacr0_mask_dacen) != 0U) {
    flags |= 0x2U;
  }
  *out_mask = flags;
  return k_ra8_ok;
}

ra8_err_t ra8_dac_b_clear_status(void)
{
  internal_stop_channel(k_ra8_dac_b_channel_0);
  internal_stop_channel(k_ra8_dac_b_channel_1);
  return k_ra8_ok;
}

ra8_err_t ra8_dac_b_attach_handler(ra8_dac_b_update_fn_t fn, void* ctx)
{
  s_dac_b_state.fn  = fn;
  s_dac_b_state.ctx = ctx;
  return k_ra8_ok;
}

ra8_err_t ra8_dac_b_enter_stop(void)
{
  internal_disable_channel(k_ra8_dac_b_channel_0);
  internal_disable_channel(k_ra8_dac_b_channel_1);
  (void)ra8_mstp_disable(k_ra8_mstp_dac12_1);
  return ra8_mstp_disable(k_ra8_mstp_dac12_0);
}

ra8_err_t ra8_dac_b_exit_stop(void)
{
  const ra8_err_t err0 = ra8_mstp_enable(k_ra8_mstp_dac12_0);
  /* GCOVR_EXCL_BR_START -- MSTP HW readback */
  RA8_RETURN_ON_ERROR(err0, s_tag, "dac_b_exit_stop: mstp0");
  /* GCOVR_EXCL_BR_STOP */
  return ra8_mstp_enable(k_ra8_mstp_dac12_1);
}

RA8_ISR_SAFE
void ra8_dac_b_dispatch_update(uint8_t channel)
{
  if ((uint16_t)channel >= k_ra8_dac_b_channel_count) {
    return;
  }
  const ra8_dac_b_update_fn_t fn  = s_dac_b_state.fn;
  void* const                 ctx = s_dac_b_state.ctx;
  if (fn != nullptr) {
    fn(ctx, channel);
  }
}
