/**
 * @file iic.c
 * @brief Full-featured I2C (IIC) driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Wave 3.2 full build-out. See ``ra_iic.h`` for the API contract.
 * Owns every write to the RA8D2 IIC register block (HUM Ch 39
 * "I2C Bus Interface (IIC)", p 2367).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8d2_iic_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_iic.h"
#include "ra_log.h"
#include "ra_mstp.h"

static const char* s_tag = "IIC";

/**
 * @enum ra_iic_icsr2_bit_t
 * @brief ICSR2 status bit positions referenced by the polling loop.
 */
typedef enum : uint8_t {
  k_ra_icsr2_bit_tmof  = 0U, /**< Timeout detection.            */
  k_ra_icsr2_bit_al    = 1U, /**< Arbitration lost.             */
  k_ra_icsr2_bit_start = 2U, /**< Start condition detected.     */
  k_ra_icsr2_bit_stop  = 3U, /**< Stop  condition detected.     */
  k_ra_icsr2_bit_nackf = 4U, /**< NACK detection.               */
  k_ra_icsr2_bit_rdrf  = 5U, /**< RX data full.                 */
  k_ra_icsr2_bit_teof  = 6U, /**< TX end.                        */
  k_ra_icsr2_bit_tdre  = 7U, /**< TX data empty.                 */
} ra_iic_icsr2_bit_t;

/**
 * @enum ra_iic_iccr2_bit_t
 * @brief ICCR2 control bit positions used by the polling driver.
 */
typedef enum : uint8_t {
  k_ra_iccr2_bit_st = 1U, /**< Start trigger.   */
  k_ra_iccr2_bit_rs = 2U, /**< Restart trigger. */
  k_ra_iccr2_bit_sp = 3U, /**< Stop  trigger.   */
} ra_iic_iccr2_bit_t;

/**
 * @enum ra_iic_init_val_t
 * @brief Magic register values used during IIC init (from HUM 31.2).
 */
typedef enum : uint8_t {
  k_ra_iic_iccr1_enable  = 0x80U, /**< ICE bit set, controller mode. */
  k_ra_iic_icmr1_default = 0x08U, /**< Default 9-bit internal counter. */
} ra_iic_init_val_t;

/**
 * @var s_iic_mstp_table
 * @brief Channel-index -> MSTP id lookup. Indexed by ``channel``.
 *        Size pinned by ``k_ra_iic_channel_count`` from
 *        ``ra8d2_iic_regs.h``.
 */
static const ra_mstp_t s_iic_mstp_table[k_ra_iic_channel_count] = {
  /* HUM Ch 11.2.7 "MSTPCRB : Module Stop Control Register B", p 444 */
  k_ra_mstp_iic0,
  k_ra_mstp_iic1,
  k_ra_mstp_iic2,
};

/**
 * @struct ra_iic_state_t
 * @brief Per-channel dispatch state.
 */
typedef struct {
  ra_iic_complete_fn_t cb;          /**< Transfer-complete callback. */
  void*                ctx;         /**< Callback context.           */
  bool                 initialised; /**< Tracked by ra_iic_init.     */
} ra_iic_state_t;

/**
 * @var s_iic_state
 * @brief Per-channel state table indexed by channel.
 */
static ra_iic_state_t s_iic_state[k_ra_iic_channel_count];

/**
 * @brief Compute ICBR (low byte) from target bus clock + PCLKB.
 *
 * @details
 * Standard formula from HUM Ch 39.2 "IIC Bit Rate Calculation":
 *
 * @f[
 *    ICBRL = \frac{PCLKB}{8 \cdot B} - 1
 * @f]
 *
 * for the default CKS = 000 (no prescaler). Caller clamps the
 * result to a valid 5-bit range before writing.
 */
static uint8_t internal_icbrl(uint32_t bus_hz, uint32_t pclkb_hz)
{
  if ((bus_hz == 0U) || (pclkb_hz == 0U)) {
    return 0U;
  }
  const uint32_t divisor = 8U * bus_hz;
  const uint32_t n       = pclkb_hz / divisor;
  if (n == 0U) {
    return 0U;
  }
  return (uint8_t)(n - 1U);
}

ra_err_t ra_iic_init(uint8_t channel, const ra_iic_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "iic_init: cfg");
  volatile r_iic_regs_t* reg = ra_iic(channel);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  if ((uint16_t)channel >= (uint16_t)k_ra_iic_channel_count) {
    return k_ra_err_invalid_arg;
  }

  /* HUM Ch 11.2.7 "MSTPCRB : Module Stop Control Register B", p 444 */
  const ra_err_t mst_err = ra_mstp_enable(s_iic_mstp_table[channel]);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "iic_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  /* HUM Ch 39.2 "IIC Bus Interface Register Descriptions", p 2367
   *  -- module-reset sequence. */
  reg->ICCR1 = 0U;
  reg->ICMR1 = (uint8_t)k_ra_iic_icmr1_default;
  reg->ICMR2 = 0U;
  reg->ICMR3 = 0U;
  reg->ICFER = 0U;
  reg->ICSER = 0U;
  reg->ICIER = 0U;
  reg->ICBRL = internal_icbrl(cfg->bus_hz, cfg->pclkb_hz);
  reg->ICBRH = 0U;
  reg->ICCR1 = (uint8_t)k_ra_iic_iccr1_enable;

  s_iic_state[channel].cb          = nullptr;
  s_iic_state[channel].ctx         = nullptr;
  s_iic_state[channel].initialised = true;
  ra_log_info_val(s_tag, "iic_init channel", (uint32_t)channel);
  return k_ra_ok;
}

ra_err_t ra_iic_deinit(uint8_t channel)
{
  volatile r_iic_regs_t* reg = ra_iic(channel);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 39.2 "ICCR1 : IIC Bus Control Register 1", p 2367 */
  reg->ICCR1                       = 0U;
  s_iic_state[channel].cb          = nullptr;
  s_iic_state[channel].ctx         = nullptr;
  s_iic_state[channel].initialised = false;
  return ra_mstp_disable(s_iic_mstp_table[channel]);
}

ra_err_t ra_iic_controller_init(uint8_t channel)
{
  /* Legacy shim: call the new init with a placeholder config.
   * Callers that need a specific bit rate should switch to
   * ra_iic_init directly. */
  const ra_iic_cfg_t cfg = {
    .bus_hz   = (uint32_t)k_ra_iic_speed_standard,
    .pclkb_hz = 60000000U,
  };
  return ra_iic_init(channel, &cfg);
}

/**
 * @brief Wait for a status flag in ICSR2.
 */
static ra_err_t internal_wait_icsr2(volatile r_iic_regs_t* reg, uint8_t bit)
{
  enum : uint32_t {
    k_ra_iic_poll_limit = 200000U,
  };
  for (uint32_t i = 0U; i < k_ra_iic_poll_limit; i++) {
    if ((reg->ICSR2 & (uint8_t)(1U << bit)) != 0U) {
      return k_ra_ok;
    }
  }
  return k_ra_err_hw_timeout;
}

ra_err_t ra_iic_write(uint8_t channel, uint8_t target_7b, const uint8_t* data, uint32_t len)
{
  volatile r_iic_regs_t* reg = ra_iic(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel out of range");
  RA_CHECK_NULL_PTR(data, s_tag, "data must not be nullptr");

  /* Issue start. */
  reg->ICCR2 = (uint8_t)(1U << k_ra_iccr2_bit_st);

  /* Wait for start detect. */
  ra_err_t err = internal_wait_icsr2(reg, (uint8_t)k_ra_icsr2_bit_start);
  if (err != k_ra_ok) {
    return err;
  }

  /* Clear start flag. */
  reg->ICSR2 = (uint8_t)~(uint8_t)(1U << k_ra_icsr2_bit_start);

  /* Address + W bit. */
  reg->ICDRT = (uint8_t)((uint8_t)(target_7b << 1U) | 0U);

  /* Data. */
  for (uint32_t i = 0U; i < len; i++) {
    err = internal_wait_icsr2(reg, (uint8_t)k_ra_icsr2_bit_tdre);
    if (err != k_ra_ok) {
      break;
    }
    reg->ICDRT = data[i];
  }

  /* Stop. */
  reg->ICSR2 = (uint8_t)~(uint8_t)(1U << k_ra_icsr2_bit_stop);
  reg->ICCR2 = (uint8_t)(1U << k_ra_iccr2_bit_sp);
  (void)internal_wait_icsr2(reg, (uint8_t)k_ra_icsr2_bit_stop);
  reg->ICSR2 = 0U;

  return err;
}

ra_err_t ra_iic_read(uint8_t channel, uint8_t target_7b, uint8_t* out, uint32_t len)
{
  volatile r_iic_regs_t* reg = ra_iic(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel out of range");
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");

  /* Issue start. */
  reg->ICCR2   = (uint8_t)(1U << k_ra_iccr2_bit_st);
  ra_err_t err = internal_wait_icsr2(reg, (uint8_t)k_ra_icsr2_bit_start);
  if (err != k_ra_ok) {
    return err;
  }
  reg->ICSR2 = (uint8_t)~(uint8_t)(1U << k_ra_icsr2_bit_start);

  /* Address + R bit. */
  reg->ICDRT = (uint8_t)((uint8_t)(target_7b << 1U) | 1U);

  for (uint32_t i = 0U; i < len; i++) {
    err = internal_wait_icsr2(reg, (uint8_t)k_ra_icsr2_bit_rdrf);
    if (err != k_ra_ok) {
      break;
    }
    out[i] = reg->ICDRR;
  }

  reg->ICCR2 = (uint8_t)(1U << k_ra_iccr2_bit_sp);
  (void)internal_wait_icsr2(reg, (uint8_t)k_ra_icsr2_bit_stop);
  reg->ICSR2 = 0U;

  return err;
}

/* =============================================================================
 * Wave 3.2: runtime reconfigure, error status, IRQ dispatch, power transition
 * =============================================================================
 */

ra_err_t ra_iic_set_clock(uint8_t channel, uint32_t bus_hz, uint32_t pclkb_hz)
{
  volatile r_iic_regs_t* reg = ra_iic(channel);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  if (bus_hz == 0U) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 39.2 "ICBRL : IIC Bit Rate Low-Level Register", p 2391 */
  reg->ICBRL = internal_icbrl(bus_hz, pclkb_hz);
  return k_ra_ok;
}

ra_err_t ra_iic_get_errors(uint8_t channel, uint8_t* out_mask)
{
  RA_CHECK_NULL_PTR(out_mask, s_tag, "get_errors: out");
  volatile const r_iic_regs_t* reg = ra_iic(channel);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  const uint8_t ss = reg->ICSR2;
  uint8_t       m  = (uint8_t)k_ra_iic_err_none;
  if ((ss & (uint8_t)(1U << (uint8_t)k_ra_icsr2_bit_al)) != 0U) {
    m |= (uint8_t)k_ra_iic_err_arb_lost;
  }
  if ((ss & (uint8_t)(1U << (uint8_t)k_ra_icsr2_bit_nackf)) != 0U) {
    m |= (uint8_t)k_ra_iic_err_nack;
  }
  if ((ss & (uint8_t)(1U << (uint8_t)k_ra_icsr2_bit_tmof)) != 0U) {
    m |= (uint8_t)k_ra_iic_err_timeout;
  }
  *out_mask = m;
  return k_ra_ok;
}

ra_err_t ra_iic_clear_errors(uint8_t channel)
{
  volatile r_iic_regs_t* reg = ra_iic(channel);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 39.2 "ICSR2 : IIC Bus Status Register 2", p 2384
   *  -- AL / NACKF / TMOF are write-zero-to-clear. */
  const uint8_t clr_mask =
    (uint8_t)~((1U << (uint8_t)k_ra_icsr2_bit_al) | (1U << (uint8_t)k_ra_icsr2_bit_nackf) |
               (1U << (uint8_t)k_ra_icsr2_bit_tmof));
  reg->ICSR2 = (uint8_t)(reg->ICSR2 & clr_mask);
  return k_ra_ok;
}

/**
 * @enum ra_iic_icier_t
 * @brief ICIER defaults used when attaching a transfer handler.
 */
typedef enum : uint8_t {
  k_ra_iic_icier_all_en = 0xFFU, /**< Enable every interrupt source. */
} ra_iic_icier_t;

ra_err_t ra_iic_attach_transfer_handler(uint8_t channel, ra_iic_complete_fn_t fn, void* ctx)
{
  volatile r_iic_regs_t* reg = ra_iic(channel);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  s_iic_state[channel].cb  = fn;
  s_iic_state[channel].ctx = ctx;
  /* HUM Ch 39.2 "ICIER : IIC Bus Interrupt Enable Register", p 2381
   *  -- enable or disable every IRQ source as a block. Per-bit
   *  tuning lands in Wave 3.2b when the first interrupt-mode
   *  consumer arrives. */
  if (fn != nullptr) {
    reg->ICIER = (uint8_t)k_ra_iic_icier_all_en;
  } else {
    reg->ICIER = 0U;
  }
  return k_ra_ok;
}

ra_err_t ra_iic_enter_stop(uint8_t channel)
{
  volatile r_iic_regs_t* reg = ra_iic(channel);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  /* Clear ICE so the controller stops.
   * HUM Ch 39.2 "ICCR1 : IIC Bus Control Register 1", p 2369 */
  reg->ICCR1 = 0U;
  return ra_mstp_disable(s_iic_mstp_table[channel]);
}

ra_err_t ra_iic_exit_stop(uint8_t channel)
{
  if ((uint16_t)channel >= (uint16_t)k_ra_iic_channel_count) {
    return k_ra_err_invalid_arg;
  }
  return ra_mstp_enable(s_iic_mstp_table[channel]);
}

void ra_iic_dispatch_txi(uint8_t channel)
{
  if ((uint16_t)channel >= (uint16_t)k_ra_iic_channel_count) {
    return;
  }
  /* Wave 3.2 MVP: the full state machine lives in the synchronous
   * write/read paths above. IRQ-mode advance is Wave 3.2b. */
  (void)s_iic_state[channel].cb;
}

void ra_iic_dispatch_rxi(uint8_t channel)
{
  if ((uint16_t)channel >= (uint16_t)k_ra_iic_channel_count) {
    return;
  }
  (void)s_iic_state[channel].cb;
}

void ra_iic_dispatch_eri(uint8_t channel)
{
  if ((uint16_t)channel >= (uint16_t)k_ra_iic_channel_count) {
    return;
  }
  uint8_t mask = 0U;
  (void)ra_iic_get_errors(channel, &mask);
  (void)ra_iic_clear_errors(channel);
  const ra_iic_complete_fn_t cb = s_iic_state[channel].cb;
  if ((mask != 0U) && (cb != nullptr)) {
    cb(s_iic_state[channel].ctx, mask);
  }
}
