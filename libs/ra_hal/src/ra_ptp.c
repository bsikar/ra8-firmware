/**
 * @file ra_ptp.c
 * @brief IEEE 1588 Precision Time Protocol driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Implementation peer to ``ra_eth_gptp.c``. ``ra_eth_gptp`` owns the
 * raw GPTP timer + IRQ surface; this file owns the SYNFP / STCA layer
 * that turns the timer into a IEEE 1588-2019 compliant ordinary or
 * boundary clock. Every register access cites either HUM Ch 35
 * "Ethernet Generic PTP Timer (GPTP)" p 1925 or the IEEE 1588-2019
 * specification clause it implements.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_ptp.h"

#include <stdint.h>

#include "ra8d2_ptp_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_mstp.h"

static const char* s_tag = "RA_PTP";

/**
 * @enum ra_ptp_state_t
 * @brief Internal lifecycle state.
 */
typedef enum : uint8_t {
  k_ra_ptp_state_closed = 0U, /**< Driver not open. */
  k_ra_ptp_state_open   = 1U, /**< Driver open.     */
} ra_ptp_state_t;

/**
 * @enum ra_ptp_mac_idx_t
 * @brief Indexes into the 6-byte MAC address.
 */
typedef enum : uint8_t {
  k_ra_ptp_mac_idx_0 = 0U,
  k_ra_ptp_mac_idx_1 = 1U,
  k_ra_ptp_mac_idx_2 = 2U,
  k_ra_ptp_mac_idx_3 = 3U,
  k_ra_ptp_mac_idx_4 = 4U,
  k_ra_ptp_mac_idx_5 = 5U,
} ra_ptp_mac_idx_t;

/**
 * @enum ra_ptp_byte_shift_t
 * @brief Big-endian byte-shift constants for MAC packing.
 */
typedef enum : uint8_t {
  k_ra_ptp_shift_b0 = 0U,
  k_ra_ptp_shift_b1 = 8U,
  k_ra_ptp_shift_b2 = 16U,
  k_ra_ptp_shift_b3 = 24U,
} ra_ptp_byte_shift_t;

/**
 * @var s_state
 * @brief Driver lifecycle state.
 */
static ra_ptp_state_t s_state = k_ra_ptp_state_closed;

/**
 * @var s_role
 * @brief Currently selected port role.
 */
static ra_ptp_role_t s_role = k_ra_ptp_role_master;

/**
 * @var s_msg_fn
 * @brief Registered message-arrival callback.
 */
static ra_ptp_msg_fn_t s_msg_fn;

/**
 * @var s_msg_ctx
 * @brief Context delivered to ``s_msg_fn``.
 */
static void* s_msg_ctx;

/**
 * @brief Pack 4 MAC address bytes into a 32-bit register word.
 *
 * @details See implementation.
 * @param[in] b0 See implementation.
 * @param[in] b1 See implementation.
 * @param[in] b2 See implementation.
 * @param[in] b3 See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static inline uint32_t internal_pack_mac4(uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3)
{
  return ((uint32_t)b0 << k_ra_ptp_shift_b0) | ((uint32_t)b1 << k_ra_ptp_shift_b1) |
         ((uint32_t)b2 << k_ra_ptp_shift_b2) | ((uint32_t)b3 << k_ra_ptp_shift_b3);
}

ra_err_t ra_ptp_open(const ra_ptp_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");
  if ((uint16_t)cfg->domain > (uint16_t)k_ra_ptp_domain_user_max) {
    return k_ra_err_invalid_arg;
  }

  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C" p 446 */
  const ra_err_t mst_err = ra_mstp_enable(k_ra_mstp_eswm);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "ptp_open: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  volatile r_ptp_regs_t* reg = ra_ptp_regs();
  /* HUM Ch 35 "Ethernet Generic PTP Timer (GPTP)" p 1925 */
  reg->PTP_CTRL   = 0UL;
  reg->PTP_DOMAIN = (uint32_t)cfg->domain;
  /* IEEE 1588-2019 sec 7.7.2.3 "logMessageInterval" */
  reg->PTP_SYNINT    = (uint32_t)(uint8_t)cfg->sync_interval;
  reg->PTP_ANNINT    = (uint32_t)(uint8_t)cfg->sync_interval;
  reg->PTP_TIME_SECH = 0UL;
  reg->PTP_TIME_SECL = 0UL;
  reg->PTP_TIME_NS   = 0UL;
  reg->PTP_OFFS_NS   = 0;
  reg->PTP_RATE_PPB  = 0;

  /* IEEE 1588-2019 sec 7.5.2 "clockIdentity" -- packed from MAC. */
  reg->PTP_MAC0 = internal_pack_mac4(cfg->mac_addr[k_ra_ptp_mac_idx_0],
                                     cfg->mac_addr[k_ra_ptp_mac_idx_1],
                                     cfg->mac_addr[k_ra_ptp_mac_idx_2],
                                     cfg->mac_addr[k_ra_ptp_mac_idx_3]);
  reg->PTP_MAC1 = internal_pack_mac4(cfg->mac_addr[k_ra_ptp_mac_idx_4],
                                     cfg->mac_addr[k_ra_ptp_mac_idx_5],
                                     0U,
                                     0U);

  /* Default to master (IEEE 1588-2019 sec 9.2.5 portState=MASTER). */
  reg->PTP_CTRL = k_ra_ptp_mask_port_en;
  s_role        = k_ra_ptp_role_master;
  s_state       = k_ra_ptp_state_open;
  ra_log_info(s_tag, "ptp_open");
  return k_ra_ok;
}

ra_err_t ra_ptp_close(void)
{
  if (s_state != k_ra_ptp_state_open) {
    return k_ra_err_invalid_state;
  }
  volatile r_ptp_regs_t* reg = ra_ptp_regs();
  /* HUM Ch 35 "Ethernet Generic PTP Timer (GPTP)" p 1925 */
  reg->PTP_CTRL = 0UL;
  s_state       = k_ra_ptp_state_closed;
  s_msg_fn      = nullptr;
  s_msg_ctx     = nullptr;
  return ra_mstp_disable(k_ra_mstp_eswm);
}

ra_err_t ra_ptp_set_role(ra_ptp_role_t role)
{
  if (s_state != k_ra_ptp_state_open) {
    return k_ra_err_invalid_state;
  }
  if (role > k_ra_ptp_role_transparent_clock) {
    return k_ra_err_invalid_arg;
  }
  volatile r_ptp_regs_t* reg = ra_ptp_regs();
  /* IEEE 1588-2019 sec 9.2.5 "portState" -- ROLE[1:0] field of PTP_CTRL. */
  uint32_t ctrl = reg->PTP_CTRL;
  ctrl &= ~k_ra_ptp_mask_role;
  ctrl |= (uint32_t)role << k_ra_ptp_role_shift;
  reg->PTP_CTRL = ctrl;
  s_role        = role;
  return k_ra_ok;
}

/**
 * @brief Common helper for ``send_sync`` / ``send_announce``.
 *
 * @details See implementation.
 * @param[in] trig_mask See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static ra_err_t internal_send(uint32_t trig_mask)
{
  if (s_state != k_ra_ptp_state_open) {
    return k_ra_err_invalid_state;
  }
  if ((s_role != k_ra_ptp_role_master) && (s_role != k_ra_ptp_role_boundary_clock)) {
    return k_ra_err_invalid_state;
  }
  volatile r_ptp_regs_t* reg = ra_ptp_regs();
  /* HUM Ch 35 "Ethernet Generic PTP Timer (GPTP)" p 1925 -- write
   * trigger bit; hardware self-clears after frame queued. */
  reg->PTP_CTRL    = reg->PTP_CTRL | trig_mask;
  reg->PTP_TX_TRIG = trig_mask;
  return k_ra_ok;
}

ra_err_t ra_ptp_send_sync(void)
{
  /* IEEE 1588-2019 sec 13.6 "Sync and Delay_Req message format". */
  return internal_send(k_ra_ptp_mask_tx_sync);
}

ra_err_t ra_ptp_send_announce(void)
{
  /* IEEE 1588-2019 sec 13.5 "Announce message format". */
  return internal_send(k_ra_ptp_mask_tx_annc);
}

ra_err_t ra_ptp_get_time(uint64_t* sec, uint32_t* nsec)
{
  RA_CHECK_NULL_PTR(sec, s_tag, "sec must not be nullptr");
  RA_CHECK_NULL_PTR(nsec, s_tag, "nsec must not be nullptr");
  if (s_state != k_ra_ptp_state_open) {
    return k_ra_err_invalid_state;
  }
  volatile r_ptp_regs_t* reg = ra_ptp_regs();
  /* HUM Ch 35 "Ethernet Generic PTP Timer (GPTP)" p 1925 */
  const uint64_t hi = (uint64_t)reg->PTP_TIME_SECH;
  const uint64_t lo = (uint64_t)reg->PTP_TIME_SECL;
  *sec              = (hi << (uint64_t)k_ra_ptp_shift_b3) | lo;
  *nsec             = reg->PTP_TIME_NS;
  return k_ra_ok;
}

ra_err_t ra_ptp_set_time(uint64_t sec, uint32_t nsec)
{
  if (s_state != k_ra_ptp_state_open) {
    return k_ra_err_invalid_state;
  }
  if (nsec >= k_ra_ptp_ns_per_sec) {
    return k_ra_err_invalid_arg;
  }
  volatile r_ptp_regs_t* reg = ra_ptp_regs();
  /* HUM Ch 35 "Ethernet Generic PTP Timer (GPTP)" p 1925 */
  reg->PTP_TIME_SECH = (uint32_t)(sec >> (uint64_t)k_ra_ptp_shift_b3);
  reg->PTP_TIME_SECL = (uint32_t)sec;
  reg->PTP_TIME_NS   = nsec;
  return k_ra_ok;
}

ra_err_t ra_ptp_adjust_time(int32_t delta_ns)
{
  if (s_state != k_ra_ptp_state_open) {
    return k_ra_err_invalid_state;
  }
  volatile r_ptp_regs_t* reg = ra_ptp_regs();
  /* IEEE 1588-2019 sec 11.2 "Synchronization and syntonization" --
   * step correction stage. */
  const int64_t cur_ns = (int64_t)reg->PTP_TIME_NS + (int64_t)delta_ns;
  if (cur_ns >= 0) {
    int64_t new_ns  = cur_ns;
    int64_t carry_s = 0;
    while (new_ns >= (int64_t)k_ra_ptp_ns_per_sec) {
      new_ns -= (int64_t)k_ra_ptp_ns_per_sec;
      carry_s += 1;
    }
    reg->PTP_TIME_NS   = (uint32_t)new_ns;
    reg->PTP_TIME_SECL = reg->PTP_TIME_SECL + (uint32_t)carry_s;
  } else {
    int64_t new_ns     = cur_ns + (int64_t)k_ra_ptp_ns_per_sec;
    reg->PTP_TIME_NS   = (uint32_t)new_ns;
    reg->PTP_TIME_SECL = reg->PTP_TIME_SECL - 1U;
  }
  reg->PTP_OFFS_NS = delta_ns;
  return k_ra_ok;
}

ra_err_t ra_ptp_adjust_rate(int32_t ppb)
{
  if (s_state != k_ra_ptp_state_open) {
    return k_ra_err_invalid_state;
  }
  volatile r_ptp_regs_t* reg = ra_ptp_regs();
  /* IEEE 1588-2019 sec 11.2 "rate" stage of the slave servo. */
  reg->PTP_RATE_PPB = ppb;
  return k_ra_ok;
}

ra_err_t ra_ptp_attach_message_handler(ra_ptp_msg_fn_t fn, void* ctx)
{
  s_msg_fn  = fn;
  s_msg_ctx = ctx;
  return k_ra_ok;
}

ra_err_t ra_ptp_get_offset(int32_t* offset_ns)
{
  RA_CHECK_NULL_PTR(offset_ns, s_tag, "offset_ns must not be nullptr");
  if (s_state != k_ra_ptp_state_open) {
    return k_ra_err_invalid_state;
  }
  /* HUM Ch 35 "Ethernet Generic PTP Timer (GPTP)" p 1925 */
  *offset_ns = ra_ptp_regs()->PTP_OFFS_NS;
  return k_ra_ok;
}

void ra_ptp_dispatch_message(ra_ptp_msg_type_t type, uint64_t sec, uint32_t nsec)
{
  const ra_ptp_msg_fn_t fn  = s_msg_fn;
  void* const           ctx = s_msg_ctx;
  /* HUM Ch 35 "Ethernet Generic PTP Timer (GPTP)" p 1925 -- record
   * latched message type so a host poll path can pick it up too. */
  ra_ptp_regs()->PTP_RX_LATCH = (uint32_t)type;
  if (fn != nullptr) {
    fn(ctx, type, sec, nsec);
  }
}
