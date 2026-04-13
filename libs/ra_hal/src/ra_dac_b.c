/**
 * @file ra_dac_b.c
 * @brief 12-Bit D/A Converter (DAC12) driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Wave 4 driver for the RA8D2 DAC12 peripheral. Programmes both
 * channels (DACR/DADR0/DADR1/DADPR/DAADSCR/DAVREFCR), exposes
 * runtime write + reference-voltage selection + per-channel
 * output enable, and provides the lifecycle/status/dispatch/power
 * transition surface. Every register access carries a HUM Ch 54
 * citation.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_dac_b.h"

#include <stdint.h>

#include "ra8d2_dac_b_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_mstp.h"

static const char* s_tag = "DAC_B";

/**
 * @enum ra_dac_b_channel_t
 * @brief Named channel indices.
 */
typedef enum : uint8_t {
  k_ra_dac_b_channel_0 = 0U,
  k_ra_dac_b_channel_1 = 1U,
} ra_dac_b_channel_t;

/**
 * @brief Clamp a raw value to 12-bit range.
 *
 * @param[in] value Raw input value.
 * @return Value saturated to `[0, k_ra_dac_b_max_value]`.
 */
static inline uint16_t internal_ra_dac_b_clamp(uint16_t value)
{
  if ((uint32_t)value > (uint32_t)k_ra_dac_b_max_value) {
    return (uint16_t)k_ra_dac_b_max_value;
  }
  return value;
}

[[nodiscard]] ra_err_t ra_dac_b_init(void)
{
  /* DAC12 has two independent MSTP bits (D19/D20). The driver
   * activates both since both channel registers are programmed
   * by ra_dac_b_init.
   * HUM Ch 11.2.9 "MSTPCRD : Module Stop Control Register D", p 448 */
  ra_err_t mst_err = ra_mstp_enable(k_ra_mstp_dac12_0);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "dac_b_init: mstp dac0");
  mst_err = ra_mstp_enable(k_ra_mstp_dac12_1);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "dac_b_init: mstp dac1");

  volatile r_dac_b_regs_t* reg = ra_dac_b();
  reg->DACR                    = 0U;
  reg->DADR0                   = 0U;
  reg->DADR1                   = 0U;
  reg->DADPR                   = 0U;
  reg->DAADSCR                 = 0U;
  reg->DAVREFCR                = 0U;
  ra_log_info(s_tag, "dac_b_init");
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_dac_b_write(uint8_t channel, uint16_t value)
{
  if ((uint16_t)channel >= (uint16_t)k_ra_dac_b_channel_count) {
    return k_ra_err_invalid_arg;
  }
  volatile r_dac_b_regs_t* reg     = ra_dac_b();
  const uint16_t           clamped = internal_ra_dac_b_clamp(value);

  if (channel == (uint8_t)k_ra_dac_b_channel_0) {
    reg->DADR0 = clamped;
    reg->DACR =
      (uint8_t)(reg->DACR | (uint8_t)k_ra_dac_b_mask_dae | (uint8_t)k_ra_dac_b_mask_daoe0);
  } else {
    reg->DADR1 = clamped;
    reg->DACR =
      (uint8_t)(reg->DACR | (uint8_t)k_ra_dac_b_mask_dae | (uint8_t)k_ra_dac_b_mask_daoe1);
  }
  ra_log_info_val(s_tag, "dac_b_write value", (uint32_t)clamped);
  return k_ra_ok;
}

/* =============================================================================
 * Wave 4.2 -- full build-out
 * =============================================================================
 */

/**
 * @enum ra_dac_b_bits_t
 * @brief DACR + DAADSCR bit positions.
 */
typedef enum : uint8_t {
  k_ra_dac_b_dacr_mask_all =
    (uint8_t)k_ra_dac_b_mask_dae | (uint8_t)k_ra_dac_b_mask_daoe0 | (uint8_t)k_ra_dac_b_mask_daoe1,
  k_ra_dac_b_daadst_enable = 0x80U, /**< DAADSCR.DAADST. */
} ra_dac_b_bits_t;

/**
 * @struct ra_dac_b_state_t
 * @brief Driver-wide runtime state.
 */
typedef struct {
  ra_dac_b_update_fn_t fn;
  void*                ctx;
} ra_dac_b_state_t;

static ra_dac_b_state_t s_dac_b_state;

static uint8_t internal_cfg_to_dacr(const ra_dac_b_cfg_t* cfg)
{
  uint8_t dacr = 0U;
  if (cfg->enable_channel0 || cfg->enable_channel1) {
    dacr |= (uint8_t)k_ra_dac_b_mask_dae;
  }
  if (cfg->enable_channel0) {
    dacr |= (uint8_t)k_ra_dac_b_mask_daoe0;
  }
  if (cfg->enable_channel1) {
    dacr |= (uint8_t)k_ra_dac_b_mask_daoe1;
  }
  return dacr;
}

ra_err_t ra_dac_b_init_configured(const ra_dac_b_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR((void*)cfg, s_tag, "cfg must not be nullptr");

  ra_err_t mst_err = ra_mstp_enable(k_ra_mstp_dac12_0);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "dac_b_init_cfg: mstp dac0"); /* GCOVR_EXCL_BR_LINE */
  mst_err = ra_mstp_enable(k_ra_mstp_dac12_1);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "dac_b_init_cfg: mstp dac1"); /* GCOVR_EXCL_BR_LINE */

  volatile r_dac_b_regs_t* reg = ra_dac_b();
  reg->DADR0                   = 0U;
  reg->DADR1                   = 0U;
  reg->DADPR                   = 0U;
  reg->DAVREFCR                = (uint8_t)cfg->vref;
  if (cfg->sync_with_adc) {
    reg->DAADSCR = (uint8_t)k_ra_dac_b_daadst_enable;
  } else {
    reg->DAADSCR = 0U;
  }
  reg->DACR = internal_cfg_to_dacr(cfg);
  ra_log_info(s_tag, "dac_b_init_configured");
  return k_ra_ok;
}

ra_err_t ra_dac_b_deinit(void)
{
  volatile r_dac_b_regs_t* reg = ra_dac_b();
  reg->DACR                    = 0U;
  reg->DADR0                   = 0U;
  reg->DADR1                   = 0U;
  reg->DAVREFCR                = 0U;
  reg->DAADSCR                 = 0U;
  s_dac_b_state.fn             = nullptr;
  s_dac_b_state.ctx            = nullptr;
  (void)ra_mstp_disable(k_ra_mstp_dac12_1);
  return ra_mstp_disable(k_ra_mstp_dac12_0);
}

ra_err_t ra_dac_b_set_vref(ra_dac_b_vref_t vref)
{
  volatile r_dac_b_regs_t* reg = ra_dac_b();
  reg->DAVREFCR                = (uint8_t)vref;
  return k_ra_ok;
}

ra_err_t ra_dac_b_set_output_enable(uint8_t channel, bool enable)
{
  if ((uint16_t)channel >= (uint16_t)k_ra_dac_b_channel_count) {
    return k_ra_err_invalid_arg;
  }
  volatile r_dac_b_regs_t* reg = ra_dac_b();
  const uint8_t mask = (channel == (uint8_t)k_ra_dac_b_channel_0) ? (uint8_t)k_ra_dac_b_mask_daoe0
                                                                  : (uint8_t)k_ra_dac_b_mask_daoe1;

  if (enable) {
    reg->DACR = (uint8_t)(reg->DACR | (uint8_t)k_ra_dac_b_mask_dae | mask);
  } else {
    reg->DACR = (uint8_t)(reg->DACR & (uint8_t)~mask);
  }
  return k_ra_ok;
}

ra_err_t ra_dac_b_get_status(uint8_t* out_mask)
{
  RA_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  *out_mask = (uint8_t)(ra_dac_b()->DACR & (uint8_t)k_ra_dac_b_dacr_mask_all);
  return k_ra_ok;
}

ra_err_t ra_dac_b_clear_status(void)
{
  volatile r_dac_b_regs_t* reg = ra_dac_b();
  reg->DACR                    = (uint8_t)(reg->DACR & (uint8_t)~(uint8_t)k_ra_dac_b_dacr_mask_all);
  return k_ra_ok;
}

ra_err_t ra_dac_b_attach_handler(ra_dac_b_update_fn_t fn, void* ctx)
{
  s_dac_b_state.fn  = fn;
  s_dac_b_state.ctx = ctx;
  return k_ra_ok;
}

ra_err_t ra_dac_b_enter_stop(void)
{
  volatile r_dac_b_regs_t* reg = ra_dac_b();
  reg->DACR                    = 0U;
  (void)ra_mstp_disable(k_ra_mstp_dac12_1);
  return ra_mstp_disable(k_ra_mstp_dac12_0);
}

ra_err_t ra_dac_b_exit_stop(void)
{
  const ra_err_t err0 = ra_mstp_enable(k_ra_mstp_dac12_0);
  RA_RETURN_ON_ERROR(err0, s_tag, "dac_b_exit_stop: mstp0");
  return ra_mstp_enable(k_ra_mstp_dac12_1);
}

void ra_dac_b_dispatch_update(uint8_t channel)
{
  if ((uint16_t)channel >= (uint16_t)k_ra_dac_b_channel_count) {
    return;
  }
  const ra_dac_b_update_fn_t fn  = s_dac_b_state.fn;
  void* const                ctx = s_dac_b_state.ctx;
  if (fn != nullptr) {
    fn(ctx, channel);
  }
}
