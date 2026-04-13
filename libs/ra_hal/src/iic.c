/**
 * @file iic.c
 * @brief Polling I2C driver on top of the IIC peripheral
 *
 * @details
 * Minimum-viable controller-mode I2C driver for bring-up and sensor
 * probing. Uses the IIC register block defined in
 * `ra8d2_iic_regs.h`. No interrupts, no DMA -- `ra_iic_write()` /
 * `ra_iic_read()` busy-wait on ICSR2 flags.
 *
 * This driver is deliberately naive so the first sensor can talk
 * to the chip. A full-speed driver (with arbitration-loss recovery,
 * multi-master support, and IRQ-driven transfers) will replace it
 * later without changing the public header.
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

ra_err_t ra_iic_controller_init(uint8_t channel)
{
  volatile r_iic_regs_t* reg = ra_iic(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel out of range");
  if ((uint16_t)channel >= (uint16_t)k_ra_iic_channel_count) {
    return k_ra_err_invalid_arg;
  }

  const ra_err_t mst_err = ra_mstp_enable(s_iic_mstp_table[channel]);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "init: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  /* Module-reset sequence. */
  reg->ICCR1 = 0U;
  reg->ICMR1 = (uint8_t)k_ra_iic_icmr1_default;
  reg->ICBRL = 0U; /* Bit-rate registers -- tune per PCLKB later. */
  reg->ICBRH = 0U;
  reg->ICCR1 = (uint8_t)k_ra_iic_iccr1_enable;

  ra_log_info_val(s_tag, "init channel", (uint32_t)channel);
  return k_ra_ok;
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
