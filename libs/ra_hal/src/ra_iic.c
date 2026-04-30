/**
 * @file ra_iic.c
 * @brief Legacy IIC (RIIC) master + slave driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Polling driver mirroring FSP ``r_iic_master.c`` /
 * ``r_iic_slave.c`` with byte-wide ICCR1/ICCR2/ICMR3/ICSR2/ICDRT/ICDRR
 * registers. Flow:
 *
 *  - ``ra_iic_master_open``: ICCR1 = ICE | IICRST, drop IICRST,
 *    program ICBRH/ICBRL from ``bus_hz`` / ``pclkb_hz``,
 *    set MST + TRS in ICCR2.
 *  - ``ra_iic_master_send``:
 *      1. ST = 1 in ICCR2 (start condition).
 *      2. wait TDRE in ICSR2.
 *      3. push (target_7b<<1)|0 into ICDRT.
 *      4. for each byte: wait TDRE, write ICDRT.
 *      5. wait TEND, set SP, clear flags.
 *      6. on NACK -> set SP, return ``k_ra_err_nack``.
 *  - ``ra_iic_master_receive`` is symmetric with ACK on every byte
 *    except the last, where ICMR3.ACKBT is set.
 *  - ``ra_iic_slave_open``: SARL[0] / SARU[0] = (addr_7b << 1).
 *  - ``ra_iic_slave_send`` / ``_receive`` push/drain TX / RX registers.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_iic.h"

#include <stdint.h>

#include "ra8d2_iic_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"

static const char* s_tag = "IIC";

/**
 * @enum ra_iic_legacy_internal_t
 * @brief Internal tunables.
 */
typedef enum : uint32_t {
  k_ra_iic_legacy_spin_budget  = 50000U, /**< Bounded poll iterations. */
  k_ra_iic_legacy_min_pclkb_hz = 1000U,
  k_ra_iic_legacy_default_brg  = 0x1FU,
  k_ra_iic_legacy_max_addr_7b  = 0x7FU,
} ra_iic_legacy_internal_t;

/**
 * @brief Bounded wait until ``mask`` bits set in ICSR2.
 */
static ra_err_t internal_wait_icsr2(volatile r_iic_legacy_t* reg, uint8_t mask)
{
  for (uint32_t i = 0U; i < k_ra_iic_legacy_spin_budget; ++i) {
    const uint8_t value = reg->ICSR2;
    if ((value & k_ra_iic_legacy_icsr2_nackf) != 0U) {
      return k_ra_err_nack;
    }
    if ((value & mask) == mask) {
      return k_ra_ok;
    }
  }
  return k_ra_err_hw_timeout;
}

ra_err_t ra_iic_master_open(uint8_t channel, const ra_iic_master_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "ra_iic_master_open: cfg null");
  volatile r_iic_legacy_t* reg = ra_iic_legacy_regs(channel);
  if (reg == nullptr) {
    ra_log_error(s_tag, "ra_iic_master_open: channel out of range");
    return k_ra_err_invalid_arg;
  }
  if ((cfg->bus_hz == 0U) || (cfg->pclkb_hz < k_ra_iic_legacy_min_pclkb_hz)) {
    ra_log_error(s_tag, "ra_iic_master_open: bad clocks");
    return k_ra_err_invalid_arg;
  }

  reg->ICCR1 = (uint8_t)(k_ra_iic_legacy_iccr1_ice | k_ra_iic_legacy_iccr1_iicrst);
  reg->ICCR1 = k_ra_iic_legacy_iccr1_ice;
  /* Simple BRG: floor(pclkb_hz / (8 * bus_hz)) - 1, clamped to a 5-bit value. */
  const uint32_t denom = (uint32_t)8U * cfg->bus_hz;
  uint32_t       brg = (denom != 0U) ? ((cfg->pclkb_hz / denom) - 1U) : k_ra_iic_legacy_default_brg;
  if (brg > k_ra_iic_legacy_default_brg) {
    brg = k_ra_iic_legacy_default_brg;
  }
  reg->ICBRL = (uint8_t)brg;
  reg->ICBRH = (uint8_t)brg;
  reg->ICCR2 = (uint8_t)((uint8_t)(1U << k_ra_iic_legacy_iccr2_mst_pos) |
                         (uint8_t)(1U << k_ra_iic_legacy_iccr2_trs_pos));
  ra_log_info_val(s_tag, "ra_iic_master_open ch", (uint32_t)channel);
  return k_ra_ok;
}

ra_err_t ra_iic_master_close(uint8_t channel)
{
  volatile r_iic_legacy_t* reg = ra_iic_legacy_regs(channel);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  reg->ICCR1 = 0U;
  return k_ra_ok;
}

/**
 * @brief Common helper: issue a STOP and clear flags.
 */
static void internal_stop(volatile r_iic_legacy_t* reg)
{
  reg->ICCR2 |= (uint8_t)(1U << k_ra_iic_legacy_iccr2_sp_pos);
  reg->ICSR2 = 0U;
}

ra_err_t ra_iic_master_send(uint8_t channel, uint8_t target_7b, const uint8_t* data, uint32_t len)
{
  volatile r_iic_legacy_t* reg = ra_iic_legacy_regs(channel);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  if ((len > 0U) && (data == nullptr)) {
    ra_log_error(s_tag, "ra_iic_master_send: data null");
    return k_ra_err_null_ptr;
  }
  if (target_7b > (uint8_t)k_ra_iic_legacy_max_addr_7b) {
    return k_ra_err_invalid_arg;
  }

  reg->ICCR2 |= (uint8_t)(1U << k_ra_iic_legacy_iccr2_st_pos);
  RA_RETURN_ON_ERROR(internal_wait_icsr2(reg, k_ra_iic_legacy_icsr2_tdre),
                     s_tag,
                     "ra_iic_master_send: TDRE wait");
  reg->ICDRT = (uint8_t)(target_7b << 1U);

  for (uint32_t i = 0U; i < len; ++i) {
    const ra_err_t wait_err = internal_wait_icsr2(reg, k_ra_iic_legacy_icsr2_tdre);
    if (wait_err != k_ra_ok) {
      internal_stop(reg);
      return wait_err;
    }
    reg->ICDRT = data[i];
  }

  const ra_err_t end_err = internal_wait_icsr2(reg, k_ra_iic_legacy_icsr2_tend);
  internal_stop(reg);
  return end_err;
}

ra_err_t ra_iic_master_receive(uint8_t channel, uint8_t target_7b, uint8_t* buf, uint32_t len)
{
  volatile r_iic_legacy_t* reg = ra_iic_legacy_regs(channel);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  if ((len > 0U) && (buf == nullptr)) {
    ra_log_error(s_tag, "ra_iic_master_receive: buf null");
    return k_ra_err_null_ptr;
  }
  if (target_7b > (uint8_t)k_ra_iic_legacy_max_addr_7b) {
    return k_ra_err_invalid_arg;
  }

  reg->ICCR2 |= (uint8_t)(1U << k_ra_iic_legacy_iccr2_st_pos);
  RA_RETURN_ON_ERROR(internal_wait_icsr2(reg, k_ra_iic_legacy_icsr2_tdre),
                     s_tag,
                     "ra_iic_master_receive: TDRE wait");
  reg->ICDRT = (uint8_t)((target_7b << 1U) | 1U);

  for (uint32_t i = 0U; i < len; ++i) {
    const ra_err_t wait_err = internal_wait_icsr2(reg, k_ra_iic_legacy_icsr2_rdrf);
    if (wait_err != k_ra_ok) {
      internal_stop(reg);
      return wait_err;
    }
    buf[i] = reg->ICDRR;
  }

  internal_stop(reg);
  return k_ra_ok;
}

ra_err_t ra_iic_master_status(uint8_t channel, uint8_t* out_mask)
{
  RA_CHECK_NULL_PTR(out_mask, s_tag, "ra_iic_master_status: out_mask null");
  volatile r_iic_legacy_t* reg = ra_iic_legacy_regs(channel);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  const uint8_t icsr2 = reg->ICSR2;
  const uint8_t iccr2 = reg->ICCR2;
  uint8_t       mask  = 0U;
  if ((iccr2 & (uint8_t)(1U << k_ra_iic_legacy_iccr2_bbsy_pos)) != 0U) {
    mask |= k_ra_iic_status_busy;
  }
  if ((icsr2 & k_ra_iic_legacy_icsr2_nackf) != 0U) {
    mask |= k_ra_iic_status_nack;
  }
  if ((icsr2 & k_ra_iic_legacy_icsr2_stop) != 0U) {
    mask |= k_ra_iic_status_stop;
  }
  if ((icsr2 & k_ra_iic_legacy_icsr2_tend) != 0U) {
    mask |= k_ra_iic_status_tx_done;
  }
  if ((icsr2 & k_ra_iic_legacy_icsr2_rdrf) != 0U) {
    mask |= k_ra_iic_status_rx_full;
  }
  *out_mask = mask;
  return k_ra_ok;
}

/* =============================================================================
 * Slave surface
 * =============================================================================
 */

ra_err_t ra_iic_slave_open(uint8_t channel, const ra_iic_slave_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "ra_iic_slave_open: cfg null");
  volatile r_iic_legacy_t* reg = ra_iic_legacy_regs(channel);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  if (cfg->slave_addr_7b > (uint8_t)k_ra_iic_legacy_max_addr_7b) {
    return k_ra_err_invalid_arg;
  }

  reg->ICCR1   = (uint8_t)(k_ra_iic_legacy_iccr1_ice | k_ra_iic_legacy_iccr1_iicrst);
  reg->ICCR1   = k_ra_iic_legacy_iccr1_ice;
  reg->SARL[0] = (uint8_t)(cfg->slave_addr_7b << 1U);
  reg->SARU[0] = 0U;
  reg->ICSER   = (uint8_t)((cfg->general_call != 0U) ? 0x08U : 0x00U);
  /* Slave mode: MST=0, TRS=0. */
  reg->ICCR2 = 0U;
  return k_ra_ok;
}

ra_err_t ra_iic_slave_close(uint8_t channel)
{
  volatile r_iic_legacy_t* reg = ra_iic_legacy_regs(channel);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  reg->ICCR1   = 0U;
  reg->SARL[0] = 0U;
  reg->SARU[0] = 0U;
  return k_ra_ok;
}

ra_err_t ra_iic_slave_send(uint8_t channel, const uint8_t* data, uint32_t len)
{
  volatile r_iic_legacy_t* reg = ra_iic_legacy_regs(channel);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  if ((len > 0U) && (data == nullptr)) {
    ra_log_error(s_tag, "ra_iic_slave_send: data null");
    return k_ra_err_null_ptr;
  }
  for (uint32_t i = 0U; i < len; ++i) {
    const ra_err_t wait_err = internal_wait_icsr2(reg, k_ra_iic_legacy_icsr2_tdre);
    if (wait_err != k_ra_ok) {
      return wait_err;
    }
    reg->ICDRT = data[i];
  }
  return k_ra_ok;
}

ra_err_t ra_iic_slave_receive(uint8_t channel, uint8_t* buf, uint32_t len)
{
  volatile r_iic_legacy_t* reg = ra_iic_legacy_regs(channel);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  if ((len > 0U) && (buf == nullptr)) {
    ra_log_error(s_tag, "ra_iic_slave_receive: buf null");
    return k_ra_err_null_ptr;
  }
  for (uint32_t i = 0U; i < len; ++i) {
    const ra_err_t wait_err = internal_wait_icsr2(reg, k_ra_iic_legacy_icsr2_rdrf);
    if (wait_err != k_ra_ok) {
      return wait_err;
    }
    buf[i] = reg->ICDRR;
  }
  return k_ra_ok;
}

ra_err_t ra_iic_slave_status(uint8_t channel, uint8_t* out_mask)
{
  RA_CHECK_NULL_PTR(out_mask, s_tag, "ra_iic_slave_status: out_mask null");
  volatile r_iic_legacy_t* reg = ra_iic_legacy_regs(channel);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  const uint8_t icsr1 = reg->ICSR1;
  const uint8_t icsr2 = reg->ICSR2;
  uint8_t       mask  = 0U;
  if ((icsr1 & k_ra_iic_legacy_icsr2_aas) != 0U) {
    mask |= k_ra_iic_status_aas;
  }
  if ((icsr2 & k_ra_iic_legacy_icsr2_nackf) != 0U) {
    mask |= k_ra_iic_status_nack;
  }
  if ((icsr2 & k_ra_iic_legacy_icsr2_rdrf) != 0U) {
    mask |= k_ra_iic_status_rx_full;
  }
  if ((icsr2 & k_ra_iic_legacy_icsr2_tend) != 0U) {
    mask |= k_ra_iic_status_tx_done;
  }
  if ((icsr2 & k_ra_iic_legacy_icsr2_stop) != 0U) {
    mask |= k_ra_iic_status_stop;
  }
  *out_mask = mask;
  return k_ra_ok;
}
