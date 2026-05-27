/**
 * @file ra_ipc.c
 * @brief Inter-Processor Communication (IPC) HAL driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Drives the RA8D2 IPC mailbox between Cortex-M85 (CPU0) and
 * Cortex-M33 (CPU1). See ``ra_ipc.h`` for the public API surface.
 * Each register access carries a HUM Ch 3 citation -- IPC is not
 * power-gated by MSTPCR so the driver does not call ``ra_mstp_*``.
 *
 * The implementation covers the full HUM Ch 3 surface (p 204-233):
 * lifecycle, channel-pair convention, attribution probes, single
 * and burst FIFO send / receive with overflow protection, all eight
 * decoded IRQ lines per channel, hardware semaphore wrappers
 * (IPCSEM0..15) with test-and-set + bounded-spin take, the NMI
 * surface (IPC0NMI* / IPC1NMI*), real ISR wiring through
 * ``ra_isr_register``, and a producer/consumer ring buffer protocol
 * that uses an IPCSEM for mutual exclusion + the FIFO IRQ lines for
 * cross-core notification.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_ipc.h"

#include <stdint.h>

#include "ra8d2_elc_regs.h"
#include "ra8d2_ipc_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_isr.h"
#include "ra_log.h"

static const char* s_tag = "IPC";

/**
 * @enum ra_ipc_internal_const_t
 * @brief Magic constants used inside the driver only.
 */
typedef enum : uint32_t {
  k_ra_ipc_internal_event_full_mask =
    k_ra_ipc_event_irq0 | k_ra_ipc_event_irq1 | k_ra_ipc_event_irq2 | k_ra_ipc_event_irq3 |
    k_ra_ipc_event_irq4 | k_ra_ipc_event_irq5 | k_ra_ipc_event_irq6 | k_ra_ipc_event_irq7 |
    k_ra_ipc_event_msg_ready | k_ra_ipc_event_fifo_full | k_ra_ipc_event_err_empty |
    k_ra_ipc_event_err_full,
} ra_ipc_internal_const_t;

/**
 * @struct ra_ipc_irq_slot_t
 * @brief Per-IRQ-event-line callback slot.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  ra_ipc_irq_fn_t fn;  /**< User callback (NULL = unused).         */
  void*           ctx; /**< Opaque context handed back to ``fn``.  */
} ra_ipc_irq_slot_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @struct ra_ipc_channel_state_t
 * @brief Per-channel runtime state held by the driver.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint32_t          event_mask;                          /**< Filter mask.              */
  bool              active;                              /**< true after init.          */
  ra_ipc_irq_slot_t per_event[k_ra_ipc_irq_event_count]; /**< Per-IRQ-line callback table.   */
} ra_ipc_channel_state_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @struct ra_ipc_isr_state_t
 * @brief Per-IPC-unit ISR registration state.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  bool installed; /**< true while ``ra_ipc_install_isr`` owns the slot. */
} ra_ipc_isr_state_t;
/* cppcheck-suppress-end [unusedStructMember] */

static ra_ipc_channel_state_t s_ipc_channels[k_ra_ipc_channel_count];
static ra_ipc_event_fn_t      s_ipc_callback;
static void*                  s_ipc_context;
static ra_ipc_nmi_fn_t        s_ipc_nmi_callback;
static void*                  s_ipc_nmi_context;
static ra_ipc_isr_state_t     s_ipc_isr_state[k_ra_ipc_nmi_unit_count];

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/**
 * @brief Translate a public ``ra_ipc_event_t`` mask into the matching
 *        IPCnCLRm bit pattern.
 *
 * @details See implementation.
 * @param[in] event_mask See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static uint32_t internal_ra_ipc_event_to_clr(uint32_t event_mask)
{
  uint32_t clr = 0U;
  /* HUM Ch 3.2.14 "IPC0CLR0" p 216 -- CLR7..CLR0 share bit positions
   * with IRQ7..IRQ0 in STA. */
  clr |= (event_mask & k_ra_ipc_clr_mask_irq_all);
  if ((event_mask & k_ra_ipc_event_msg_ready) != 0U) {
    /* HUM Ch 3.2.14 "RST bit (FIFO Reset)" p 217 -- only way to
     * drop STA.RDY without consuming a word. */
    clr |= k_ra_ipc_clr_mask_rst;
  }
  if ((event_mask & k_ra_ipc_event_err_empty) != 0U) {
    /* HUM Ch 3.2.14 "RCLR bit" p 217*/
    clr |= k_ra_ipc_clr_mask_rclr;
  }
  if ((event_mask & k_ra_ipc_event_err_full) != 0U) {
    /* HUM Ch 3.2.14 "FCLR bit" p 217*/
    clr |= k_ra_ipc_clr_mask_fclr;
  }
  return clr;
}

/**
 * @brief Validate channel id and return the channel reg pointer.
 */
static volatile r_ipc_channel_regs_t* internal_ra_ipc_get_regs(uint8_t channel)
{
  if ((uint16_t)channel >= (uint16_t)k_ra_ipc_channel_count) {
    return nullptr;
  }
#ifdef RA_BUILD_FOR_CPU1
  /* CPU1 (M33) has SECEXT disabled and runs as a permanent NS bus    */ /* LEGACY-OK: ARMv8-M bus-architecture term */
  /* controller. The IPC channels CPU1 owns (ch0 + ch2) are
   * NS-attributed via IPCSAR=0x00050000 set by CPU0's secure-boot.
   * The chip routes NS IPC accesses through the bit-28-set NS alias
   * (0x50020000), not the Secure alias the M85 HAL defaults to.
   * Without this offset the first ``reg->CLR`` write inside
   * ra_ipc_init BusFaults and CPU1 wedges in cpu1_fault_handler
   * (HUM Ch 3.2 p 205 + bench probe pinning PC at 0x020C0020). */
  const uintptr_t ns_offset = 0x10000000UL;
  return (volatile r_ipc_channel_regs_t*)((uintptr_t)ra_ipc_channel(channel) + ns_offset);
#else
  return ra_ipc_channel(channel);
#endif
}

/**
 * @brief Validate NMI unit id and return the NMI reg pointer.
 */
static volatile r_ipc_nmi_regs_t* internal_ra_ipc_get_nmi(uint8_t unit)
{
  if ((uint16_t)unit >= (uint16_t)k_ra_ipc_nmi_unit_count) {
    return nullptr;
  }
  return ra_ipc_nmi(unit);
}

/**
 * @brief Read the ELC event id matching one IPC unit.
 *
 * @details
 * Returns the raw 16-bit event-list index. Callers must convert to
 * ``ra_elc_event_t`` via ``uint16_t`` at the boundary -- the enum's
 * declared list does not yet enumerate the IPC IRQ event numbers
 * (0x05B / 0x05C) and the static analyzer flags a direct cast.
 *
 * @param[in] unit See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static uint16_t internal_ra_ipc_unit_to_event(uint8_t unit)
{
  /* HUM Ch 18 "Event Link Controller" event-list -- IPC0_* feeds
   * ELC_EVENT_IPC_IRQ0 = 0x05B; IPC1_* feeds 0x05C. */
  if (unit == k_ra_ipc_unit_ipc0) {
    return k_ra_ipc_elc_event_irq0;
  }
  return k_ra_ipc_elc_event_irq1;
}

/**
 * @brief Walk every IRQn bit in ``mask`` and call the per-line
 *        callback registered for that channel/line, if any.
 *
 * @details See implementation.
 * @param[in] channel See implementation.
 * @param[in] mask See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static void internal_ra_ipc_dispatch_irq_lines(uint8_t channel, uint32_t mask)
{
  const uint32_t irq_bits = mask & k_ra_ipc_event_irq_all;
  if (irq_bits == 0U) {
    return;
  }
  ra_ipc_channel_state_t* const state = &s_ipc_channels[channel];
  for (uint8_t i = 0U; i < k_ra_ipc_irq_event_count; ++i) {
    const uint32_t bit = (uint32_t)1U << (uint32_t)i;
    if ((irq_bits & bit) == 0U) {
      continue;
    }
    const ra_ipc_irq_fn_t fn = state->per_event[i].fn;
    if (fn != nullptr) {
      fn(state->per_event[i].ctx, channel, (ra_ipc_irq_event_id_t)i);
    }
  }
}

/**
 * @brief Test-and-set on IPCSEMn -- a 32-bit read takes the lock and
 *        returns the previous LOCK value.
 *
 * @details See implementation.
 * @param[in] sem See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static uint32_t internal_ra_ipc_sem_read_take(volatile uint32_t* sem)
{
  /* HUM Ch 3.2.3 "IPCSEMn" p 210 -- "Set condition: Reading this
   * register". Reading the register sets LOCK to 1; the read result
   * is the value the register held *before* the set. */
  const uint32_t prev = *sem & k_ra_ipc_sem_mask_lock;
#ifdef RA_SIMULATOR_MODE
  /* The host-test simulator is dumb memory: a plain read has no
   * side effect, so synthesise the read-takes-lock semantics here. */
  *sem = k_ra_ipc_sem_mask_lock;
#endif
  return prev;
}

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

[[nodiscard]] ra_err_t ra_ipc_init(const ra_ipc_config_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");
  if ((uint16_t)cfg->channel >= (uint16_t)k_ra_ipc_channel_count) {
    return k_ra_err_invalid_arg;
  }

  volatile r_ipc_channel_regs_t* reg = internal_ra_ipc_get_regs(cfg->channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel mapping failed");

  if (cfg->reset_fifo) {
    /* HUM Ch 3.2.14 "IPC0CLR0" p 216 -- CLR.RST (bit 16) drops the
     * FIFO and clears STA.RDY/STA.FULL. */
    reg->CLR = k_ra_ipc_clr_mask_rst;
  }
  if (cfg->clear_status) {
    /* HUM Ch 3.2.14 "IPC0CLR0" p 216 -- ack every IRQn and both
     * RERR/FERR error flags so init returns with a clean slate. */
    reg->CLR = k_ra_ipc_clr_mask_irq_all | k_ra_ipc_clr_mask_rclr | k_ra_ipc_clr_mask_fclr;
  }

  ra_ipc_channel_state_t* const state = &s_ipc_channels[cfg->channel];
  state->event_mask                   = cfg->event_mask;
  state->active                       = true;
  for (uint8_t i = 0U; i < k_ra_ipc_irq_event_count; ++i) {
    state->per_event[i].fn  = nullptr;
    state->per_event[i].ctx = nullptr;
  }

  ra_log_info_val(s_tag, "ipc_init channel", (uint32_t)cfg->channel);
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_ipc_deinit(uint8_t channel)
{
  if ((uint16_t)channel >= (uint16_t)k_ra_ipc_channel_count) {
    return k_ra_err_invalid_arg;
  }
  volatile r_ipc_channel_regs_t* reg = internal_ra_ipc_get_regs(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel mapping failed");

  /* HUM Ch 3.2.14 "IPC0CLR0" p 216 -- clear every event bit then
   * drop the FIFO so the next init starts clean. */
  reg->CLR = k_ra_ipc_clr_mask_irq_all | k_ra_ipc_clr_mask_rst | k_ra_ipc_clr_mask_rclr |
             k_ra_ipc_clr_mask_fclr;

  ra_ipc_channel_state_t* const state = &s_ipc_channels[channel];
  state->event_mask                   = 0U;
  state->active                       = false;
  for (uint8_t i = 0U; i < k_ra_ipc_irq_event_count; ++i) {
    state->per_event[i].fn  = nullptr;
    state->per_event[i].ctx = nullptr;
  }
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_ipc_reset_fifo(uint8_t channel)
{
  volatile r_ipc_channel_regs_t* reg = internal_ra_ipc_get_regs(channel);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 3.2.14 "RST bit (FIFO Reset)" p 217 -- bit 16 drains the
   * FIFO and clears RDY/FULL. */
  reg->CLR = k_ra_ipc_clr_mask_rst;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_ipc_set_event_mask(uint8_t channel, uint32_t mask)
{
  if ((uint16_t)channel >= (uint16_t)k_ra_ipc_channel_count) {
    return k_ra_err_invalid_arg;
  }
  s_ipc_channels[channel].event_mask = mask;
  return k_ra_ok;
}

/* =============================================================================
 * Channel-pair convention helpers
 * =============================================================================
 */

[[nodiscard]] ra_err_t
ra_ipc_channel_for_send(ra_ipc_core_id_t core, uint8_t pair, uint8_t* out_channel)
{
  RA_CHECK_NULL_PTR(out_channel, s_tag, "out_channel must not be nullptr");
  if ((uint8_t)core > k_ra_ipc_core_cpu1) {
    return k_ra_err_invalid_arg;
  }
  if (pair > 1U) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 3.1 "Overview" p 204 -- CPU0 sends through IPC1 (channels
   * 2 and 3); CPU1 sends through IPC0 (channels 0 and 1). */
  if (core == k_ra_ipc_core_cpu0) {
    *out_channel = (uint8_t)(k_ra_ipc_channel_count / 2U) + pair;
  } else {
    *out_channel = pair;
  }
  return k_ra_ok;
}

[[nodiscard]] ra_err_t
ra_ipc_channel_for_recv(ra_ipc_core_id_t core, uint8_t pair, uint8_t* out_channel)
{
  RA_CHECK_NULL_PTR(out_channel, s_tag, "out_channel must not be nullptr");
  if ((uint8_t)core > k_ra_ipc_core_cpu1) {
    return k_ra_err_invalid_arg;
  }
  if (pair > 1U) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 3.1 "Overview" p 204 -- CPU0 receives from IPC0 (channels
   * 0 and 1); CPU1 receives from IPC1 (channels 2 and 3). */
  if (core == k_ra_ipc_core_cpu0) {
    *out_channel = pair;
  } else {
    *out_channel = (uint8_t)(k_ra_ipc_channel_count / 2U) + pair;
  }
  return k_ra_ok;
}

/* =============================================================================
 * Send / receive
 * =============================================================================
 */

[[nodiscard]] ra_err_t ra_ipc_send_event(uint8_t channel, ra_ipc_irq_event_id_t event_id)
{
  if ((uint16_t)channel >= (uint16_t)k_ra_ipc_channel_count) {
    return k_ra_err_invalid_arg;
  }
  if ((uint16_t)event_id >= (uint16_t)k_ra_ipc_irq_event_count) {
    return k_ra_err_invalid_arg;
  }
  volatile r_ipc_channel_regs_t* reg = internal_ra_ipc_get_regs(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel mapping failed");

  /* HUM Ch 3.2.11 "IPC0ISET0" p 215 -- write 1 to SETn to assert
   * IRQn on the receiving core and raise the IPCnIRQm interrupt. */
  reg->ISET = (uint32_t)1U << (uint32_t)event_id;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_ipc_clear_event(uint8_t channel, ra_ipc_irq_event_id_t event_id)
{
  if ((uint16_t)channel >= (uint16_t)k_ra_ipc_channel_count) {
    return k_ra_err_invalid_arg;
  }
  if ((uint16_t)event_id >= (uint16_t)k_ra_ipc_irq_event_count) {
    return k_ra_err_invalid_arg;
  }
  volatile r_ipc_channel_regs_t* reg = internal_ra_ipc_get_regs(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel mapping failed");

  /* HUM Ch 3.2.14 "CLRn bits" p 217 -- write 1 to CLRn to drop the
   * matching IRQn status bit. */
  reg->CLR = (uint32_t)1U << (uint32_t)event_id;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_ipc_send_message(uint8_t channel, uint32_t message)
{
  if ((uint16_t)channel >= (uint16_t)k_ra_ipc_channel_count) {
    return k_ra_err_invalid_arg;
  }
  volatile r_ipc_channel_regs_t* reg = internal_ra_ipc_get_regs(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel mapping failed");

  /* HUM Ch 3.2.10 "IPC0STA0 FULL bit" p 214 -- if the FIFO is full
   * the write to TXD is silently dropped and FERR is set. */
  if ((reg->STA & k_ra_ipc_sta_mask_full) != 0U) {
    return k_ra_err_busy;
  }

  /* HUM Ch 3.2.12 "IPC0TXD0" p 215 -- only 32-bit writes are valid;
   * narrower accesses are ignored by the peripheral. */
  reg->TXD = message;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t
ra_ipc_send_message_retry(uint8_t channel, uint32_t message, uint16_t max_retries)
{
  if ((uint16_t)channel >= (uint16_t)k_ra_ipc_channel_count) {
    return k_ra_err_invalid_arg;
  }
  if (max_retries > k_ra_ipc_retry_max) {
    max_retries = k_ra_ipc_retry_max;
  }
  volatile r_ipc_channel_regs_t* reg = internal_ra_ipc_get_regs(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel mapping failed");

  /* NASA Rule 2: bounded loop with the +1 covering the immediate-try
   * case where retries==0 still gets a single attempt. */
  for (uint16_t i = 0U; i <= max_retries; ++i) {
    /* HUM Ch 3.2.10 "FULL bit" p 214*/
    if ((reg->STA & k_ra_ipc_sta_mask_full) == 0U) {
      /* HUM Ch 3.2.12 "IPC0TXD0" p 215*/
      reg->TXD = message;
      return k_ra_ok;
    }
    /* HUM Ch 3.2.14 "FCLR bit" p 217 -- knock down a possible FERR
     * left over from a previous overflow so the dispatcher does not
     * spin forever on a stuck error flag. */
    reg->CLR = k_ra_ipc_clr_mask_fclr;
  }
  return k_ra_err_hw_timeout;
}

[[nodiscard]] ra_err_t
ra_ipc_send_burst(uint8_t channel, const uint32_t* data, uint32_t count, uint32_t* out_written)
{
  RA_CHECK_NULL_PTR(data, s_tag, "data must not be nullptr");
  RA_CHECK_NULL_PTR(out_written, s_tag, "out_written must not be nullptr");
  if ((uint16_t)channel >= (uint16_t)k_ra_ipc_channel_count) {
    return k_ra_err_invalid_arg;
  }
  if (count == 0U) {
    return k_ra_err_invalid_arg;
  }
  volatile r_ipc_channel_regs_t* reg = internal_ra_ipc_get_regs(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel mapping failed");

  uint32_t written = 0U;
  /* NASA Rule 2: bound is the user-supplied ``count`` and the inner
   * STA.FULL check breaks early when the 4-stage FIFO fills (HUM
   * Ch 3.1 p 204). */
  for (uint32_t i = 0U; i < count; ++i) {
    /* HUM Ch 3.2.10 "FULL bit" p 214*/
    if ((reg->STA & k_ra_ipc_sta_mask_full) != 0U) {
      break;
    }
    /* HUM Ch 3.2.12 "IPC0TXD0" p 215*/
    reg->TXD = data[i];
    ++written;
  }
  *out_written = written;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_ipc_recv_message(uint8_t channel, uint32_t* out_msg)
{
  RA_CHECK_NULL_PTR(out_msg, s_tag, "out_msg must not be nullptr");
  if ((uint16_t)channel >= (uint16_t)k_ra_ipc_channel_count) {
    return k_ra_err_invalid_arg;
  }
  volatile r_ipc_channel_regs_t* reg = internal_ra_ipc_get_regs(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel mapping failed");

  /* HUM Ch 3.2.10 "RDY bit" p 214 -- a read of RXD while RDY=0
   * returns 0 and sets RERR. */
  if ((reg->STA & k_ra_ipc_sta_mask_rdy) == 0U) {
    return k_ra_err_no_data;
  }
  /* HUM Ch 3.2.13 "IPC0RXD0" p 216*/
  *out_msg = reg->RXD;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t
ra_ipc_recv_message_retry(uint8_t channel, uint32_t* out_msg, uint16_t max_retries)
{
  RA_CHECK_NULL_PTR(out_msg, s_tag, "out_msg must not be nullptr");
  if ((uint16_t)channel >= (uint16_t)k_ra_ipc_channel_count) {
    return k_ra_err_invalid_arg;
  }
  if (max_retries > k_ra_ipc_retry_max) {
    max_retries = k_ra_ipc_retry_max;
  }
  volatile r_ipc_channel_regs_t* reg = internal_ra_ipc_get_regs(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel mapping failed");

  /* NASA Rule 2: bounded loop, max_retries clamped above. */
  for (uint16_t i = 0U; i <= max_retries; ++i) {
    /* HUM Ch 3.2.10 "RDY bit" p 214*/
    if ((reg->STA & k_ra_ipc_sta_mask_rdy) != 0U) {
      /* HUM Ch 3.2.13 "IPC0RXD0" p 216*/
      *out_msg = reg->RXD;
      return k_ra_ok;
    }
    /* HUM Ch 3.2.14 "RCLR bit" p 217 -- proactively wipe a stale
     * RERR so the next dispatch loop is not perpetually firing on
     * an old error. */
    reg->CLR = k_ra_ipc_clr_mask_rclr;
  }
  return k_ra_err_hw_timeout;
}

[[nodiscard]] ra_err_t
ra_ipc_recv_burst(uint8_t channel, uint32_t* out_data, uint32_t capacity, uint32_t* out_read)
{
  RA_CHECK_NULL_PTR(out_data, s_tag, "out_data must not be nullptr");
  RA_CHECK_NULL_PTR(out_read, s_tag, "out_read must not be nullptr");
  if ((uint16_t)channel >= (uint16_t)k_ra_ipc_channel_count) {
    return k_ra_err_invalid_arg;
  }
  if (capacity == 0U) {
    return k_ra_err_invalid_arg;
  }
  volatile r_ipc_channel_regs_t* reg = internal_ra_ipc_get_regs(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel mapping failed");

  uint32_t read_count = 0U;
  /* NASA Rule 2: bound is the caller-supplied ``capacity`` plus an
   * inner break when STA.RDY drops. */
  for (uint32_t i = 0U; i < capacity; ++i) {
    /* HUM Ch 3.2.10 "RDY bit" p 214*/
    if ((reg->STA & k_ra_ipc_sta_mask_rdy) == 0U) {
      break;
    }
    /* HUM Ch 3.2.13 "IPC0RXD0" p 216*/
    out_data[i] = reg->RXD;
    ++read_count;
  }
  *out_read = read_count;
  return k_ra_ok;
}

/* =============================================================================
 * Status / errors
 * =============================================================================
 */

[[nodiscard]] ra_err_t ra_ipc_get_status(uint8_t channel, uint32_t* out_sta)
{
  RA_CHECK_NULL_PTR(out_sta, s_tag, "out_sta must not be nullptr");
  if ((uint16_t)channel >= (uint16_t)k_ra_ipc_channel_count) {
    return k_ra_err_invalid_arg;
  }
  volatile r_ipc_channel_regs_t* reg = internal_ra_ipc_get_regs(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel mapping failed");

  /* HUM Ch 3.2.10 "IPC0STA0" p 214*/
  *out_sta = reg->STA;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_ipc_clear_status(uint8_t channel, uint32_t mask)
{
  if ((uint16_t)channel >= (uint16_t)k_ra_ipc_channel_count) {
    return k_ra_err_invalid_arg;
  }
  volatile r_ipc_channel_regs_t* reg = internal_ra_ipc_get_regs(channel);
  RA_CHECK_NULL_PTR(reg, s_tag, "channel mapping failed");

  const uint32_t clr = internal_ra_ipc_event_to_clr(mask);
  if (clr != 0U) {
    /* HUM Ch 3.2.14 "IPC0CLR0" p 216*/
    reg->CLR = clr;
  }
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_ipc_clear_errors(uint8_t channel)
{
  volatile r_ipc_channel_regs_t* reg = internal_ra_ipc_get_regs(channel);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 3.2.14 "RCLR bit / FCLR bit" p 217 -- ack both error
   * flags in one CLR write. */
  reg->CLR = k_ra_ipc_clr_mask_rclr | k_ra_ipc_clr_mask_fclr;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_ipc_can_send(uint8_t channel, bool* out_can_send)
{
  RA_CHECK_NULL_PTR(out_can_send, s_tag, "out_can_send must not be nullptr");
  volatile r_ipc_channel_regs_t* reg = internal_ra_ipc_get_regs(channel);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 3.2.10 "FULL bit" p 214*/
  *out_can_send = ((reg->STA & k_ra_ipc_sta_mask_full) == 0U);
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_ipc_has_data(uint8_t channel, bool* out_has_data)
{
  RA_CHECK_NULL_PTR(out_has_data, s_tag, "out_has_data must not be nullptr");
  volatile r_ipc_channel_regs_t* reg = internal_ra_ipc_get_regs(channel);
  if (reg == nullptr) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 3.2.10 "RDY bit" p 214*/
  *out_has_data = ((reg->STA & k_ra_ipc_sta_mask_rdy) != 0U);
  return k_ra_ok;
}

/* =============================================================================
 * Security / privilege attribution
 * =============================================================================
 */

[[nodiscard]] ra_err_t ra_ipc_get_attribution(uint8_t channel, ra_ipc_attr_t* out_attr)
{
  RA_CHECK_NULL_PTR(out_attr, s_tag, "out_attr must not be nullptr");
  if ((uint16_t)channel >= (uint16_t)k_ra_ipc_channel_count) {
    return k_ra_err_invalid_arg;
  }

  /* HUM Ch 3.2.1 "IPCSAR" p 205 / Ch 3.2.2 "IPCPAR" p 208 -- the
   * SAIPCIRn / PAIPCIRn bits sit at [16+n] in their respective 32-bit
   * registers. */
  const uint32_t sar = *ra_ipc_ipcsar();
  const uint32_t par = *ra_ipc_ipcpar();
  const uint32_t bit = (uint32_t)1U << ((uint32_t)k_ra_ipc_attr_shift_ir_base + (uint32_t)channel);

  out_attr->secure     = ((sar & bit) != 0U);
  out_attr->privileged = ((par & bit) != 0U);
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_ipc_get_nmi_attribution(uint8_t unit, ra_ipc_attr_t* out_attr)
{
  RA_CHECK_NULL_PTR(out_attr, s_tag, "out_attr must not be nullptr");
  if ((uint16_t)unit >= (uint16_t)k_ra_ipc_nmi_unit_count) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 3.2.1 "IPCSAR.SAIPCNMIu bits" p 205-207 -- SAIPCNMI0 at bit 8,
   * SAIPCNMI1 at bit 9; PAIPCNMI mirrors the same positions. */
  const uint32_t sar   = *ra_ipc_ipcsar();
  const uint32_t par   = *ra_ipc_ipcpar();
  const uint32_t bit   = (uint32_t)1U << ((uint32_t)k_ra_ipc_attr_shift_nmi_base + (uint32_t)unit);
  out_attr->secure     = ((sar & bit) != 0U);
  out_attr->privileged = ((par & bit) != 0U);
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_ipc_get_sem_attribution(ra_ipc_sem_attr_group_t group,
                                                  ra_ipc_attr_t*          out_attr)
{
  RA_CHECK_NULL_PTR(out_attr, s_tag, "out_attr must not be nullptr");
  if ((uint8_t)group > k_ra_ipc_sem_group_high) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 3.2.1 "IPCSAR.SAIPCSEMg" p 205 */
  const uint32_t sar   = *ra_ipc_ipcsar();
  const uint32_t par   = *ra_ipc_ipcpar();
  const uint32_t bit   = (uint32_t)1U << (uint32_t)group;
  out_attr->secure     = ((sar & bit) != 0U);
  out_attr->privileged = ((par & bit) != 0U);
  return k_ra_ok;
}

[[nodiscard]] ra_err_t
ra_ipc_can_access(uint8_t channel, ra_ipc_attr_t const* required, bool* out_can_access)
{
  RA_CHECK_NULL_PTR(required, s_tag, "required must not be nullptr");
  RA_CHECK_NULL_PTR(out_can_access, s_tag, "out_can_access must not be nullptr");
  ra_ipc_attr_t  live = {.secure = false, .privileged = false};
  const ra_err_t err  = ra_ipc_get_attribution(channel, &live);
  if (err != k_ra_ok) {
    return err;
  }
  const bool secure_match     = live.secure == required->secure;
  const bool privileged_match = live.privileged == required->privileged;
  *out_can_access             = (bool)(secure_match && privileged_match);
  return k_ra_ok;
}

/* =============================================================================
 * Hardware semaphores
 * =============================================================================
 */

[[nodiscard]] ra_err_t ra_ipc_sem_try_take(uint8_t sem_id)
{
  if ((uint16_t)sem_id >= (uint16_t)k_ra_ipc_sem_count) {
    return k_ra_err_invalid_arg;
  }
  volatile uint32_t* sem = ra_ipc_sem(sem_id);
  RA_CHECK_NULL_PTR(sem, s_tag, "sem mapping failed");
  /* HUM Ch 3.2.3 "IPCSEMn" p 210 -- 32-bit read sets LOCK; the read
   * value reports the *previous* state. 0 -> we just acquired,
   * 1 -> someone else already owned it. */
  const uint32_t prev = internal_ra_ipc_sem_read_take(sem);
  if (prev != 0U) {
    return k_ra_err_busy;
  }
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_ipc_sem_take_timeout(uint8_t sem_id, uint16_t max_spins)
{
  if ((uint16_t)sem_id >= (uint16_t)k_ra_ipc_sem_count) {
    return k_ra_err_invalid_arg;
  }
  if (max_spins == 0U) {
    return k_ra_err_invalid_arg;
  }
  if (max_spins > k_ra_ipc_sem_take_max) {
    max_spins = k_ra_ipc_sem_take_max;
  }
  volatile uint32_t* sem = ra_ipc_sem(sem_id);
  RA_CHECK_NULL_PTR(sem, s_tag, "sem mapping failed");
  /* NASA Rule 2: bounded by ``max_spins``. */
  for (uint16_t i = 0U; i < max_spins; ++i) { /* GCOVR_EXCL_BR_LINE */
    /* HUM Ch 3.2.3 "IPCSEMn" p 210 */
    const uint32_t prev = internal_ra_ipc_sem_read_take(sem);
    if (prev == 0U) { /* GCOVR_EXCL_BR_LINE */
      return k_ra_ok;
    }
  }
  return k_ra_err_hw_timeout;
}

[[nodiscard]] ra_err_t ra_ipc_sem_release(uint8_t sem_id)
{
  if ((uint16_t)sem_id >= (uint16_t)k_ra_ipc_sem_count) {
    return k_ra_err_invalid_arg;
  }
  volatile uint32_t* sem = ra_ipc_sem(sem_id);
  RA_CHECK_NULL_PTR(sem, s_tag, "sem mapping failed");
  /* HUM Ch 3.2.3 "IPCSEMn" p 210 -- "Clear condition: Writing 1 to
   * this bit". */
  *sem = k_ra_ipc_sem_mask_lock;
#ifdef RA_SIMULATOR_MODE
  /* On real HW, writing 1 clears LOCK to 0. The host-test simulator
   * is dumb memory and just stores the 1; reflect the released state
   * so subsequent take/is_locked observations match HW. */
  *sem = 0U;
#endif
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_ipc_sem_is_locked(uint8_t sem_id, bool* out_locked)
{
  RA_CHECK_NULL_PTR(out_locked, s_tag, "out_locked must not be nullptr");
  if ((uint16_t)sem_id >= (uint16_t)k_ra_ipc_sem_count) {
    return k_ra_err_invalid_arg;
  }
  volatile uint32_t* sem = ra_ipc_sem(sem_id);
  RA_CHECK_NULL_PTR(sem, s_tag, "sem mapping failed");
  /* HUM Ch 3.2.3 "IPCSEMn" p 210 -- the 32-bit read takes the lock
   * as a side-effect, so we sample then restore the previous state. */
  const uint32_t prev = internal_ra_ipc_sem_read_take(sem);
  *out_locked         = (prev != 0U);
  if (prev == 0U) {
    /* Caller observed unlocked; the read just locked it -- HUM
     * Ch 3.2.3 "IPCSEMn" p 210 says writing 1 to LOCK clears the bit
     * (releases the resource), and the post-condition for a diagnostic
     * predicate is that the register reads back 0 so the side-effect
     * of the take is invisible. The first write performs the
     * write-1-to-clear; the second drives the value the caller would
     * observe via a non-acquiring read on real silicon (LOCK=0). On
     * the dumb-memory unit-test sim the second write is the one that
     * is actually observable. */
    *sem = k_ra_ipc_sem_mask_lock;
    *sem = 0U;
  }
  return k_ra_ok;
}

/* =============================================================================
 * NMI surface
 * =============================================================================
 */

[[nodiscard]] ra_err_t ra_ipc_nmi_send(uint8_t unit)
{
  volatile r_ipc_nmi_regs_t* nmi = internal_ra_ipc_get_nmi(unit);
  if (nmi == nullptr) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 3.2.5 "IPC0NMISET" p 211 / Ch 3.2.8 "IPC1NMISET" p 213 --
   * write 1 to SET to assert NMI on the peer. */
  nmi->NMISET = k_ra_ipc_nmi_mask_bit;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_ipc_nmi_clear(uint8_t unit)
{
  volatile r_ipc_nmi_regs_t* nmi = internal_ra_ipc_get_nmi(unit);
  if (nmi == nullptr) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 3.2.6 "IPC0NMICLR" p 212 / Ch 3.2.9 "IPC1NMICLR" p 213 --
   * write 1 to CLR to drop NMISTA.NMI. */
  nmi->NMICLR = k_ra_ipc_nmi_mask_bit;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_ipc_nmi_get_status(uint8_t unit, bool* out_pending)
{
  RA_CHECK_NULL_PTR(out_pending, s_tag, "out_pending must not be nullptr");
  volatile r_ipc_nmi_regs_t* nmi = internal_ra_ipc_get_nmi(unit);
  if (nmi == nullptr) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 3.2.4 "IPC0NMISTA.NMI" p 210 */
  *out_pending = ((nmi->NMISTA & k_ra_ipc_nmi_mask_bit) != 0U);
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_ipc_attach_nmi_handler(ra_ipc_nmi_fn_t fn, void* ctx)
{
  s_ipc_nmi_callback = fn;
  s_ipc_nmi_context  = ctx;
  return k_ra_ok;
}

/* Implementation of ra_ipc_dispatch_nmi (see header for full contract) -- see header for the documented contract. */
void ra_ipc_dispatch_nmi(uint8_t unit)
{
  volatile r_ipc_nmi_regs_t* nmi = internal_ra_ipc_get_nmi(unit);
  if (nmi == nullptr) {
    return;
  }
  /* HUM Ch 3.2.4 "IPC0NMISTA.NMI" p 210 -- snapshot before invoking
   * the callback so a re-entrant SET on the peer side does not
   * confuse our acknowledge. */
  if ((nmi->NMISTA & k_ra_ipc_nmi_mask_bit) == 0U) {
    return;
  }
  const ra_ipc_nmi_fn_t fn  = s_ipc_nmi_callback;
  void* const           ctx = s_ipc_nmi_context;
  if (fn != nullptr) {
    fn(ctx, unit);
  }
  /* HUM Ch 3.2.6 "IPC0NMICLR.CLR" p 212 -- ack so the ICU drops the
   * line. */
  nmi->NMICLR = k_ra_ipc_nmi_mask_bit;
}

/* =============================================================================
 * Maskable interrupt path
 * =============================================================================
 */

[[nodiscard]] ra_err_t ra_ipc_attach_handler(ra_ipc_event_fn_t fn, void* ctx)
{
  s_ipc_callback = fn;
  s_ipc_context  = ctx;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_ipc_attach_event_handler(uint8_t               channel,
                                                   ra_ipc_irq_event_id_t event_id,
                                                   ra_ipc_irq_fn_t       fn,
                                                   void*                 ctx)
{
  if ((uint16_t)channel >= (uint16_t)k_ra_ipc_channel_count) {
    return k_ra_err_invalid_arg;
  }
  if ((uint16_t)event_id >= (uint16_t)k_ra_ipc_irq_event_count) {
    return k_ra_err_invalid_arg;
  }
  ra_ipc_irq_slot_t* const slot = &s_ipc_channels[channel].per_event[event_id];
  slot->fn                      = fn;
  slot->ctx                     = ctx;
  return k_ra_ok;
}

/* Implementation of ra_ipc_dispatch (see header for full contract) -- see header for the documented contract. */
void ra_ipc_dispatch(uint8_t channel)
{
  if ((uint16_t)channel >= (uint16_t)k_ra_ipc_channel_count) {
    return;
  }
  volatile r_ipc_channel_regs_t* reg = ra_ipc_channel(channel);
  if (reg == nullptr) {
    return; /* GCOVR_EXCL_LINE -- channel already bounded */
  }

  /* HUM Ch 3.2.10 "IPC0STA0" p 214 -- snapshot the status register
   * once so the IRQ/RDY/RERR/FERR bits do not race against the peer. */
  const uint32_t sta        = reg->STA;
  const uint32_t configured = s_ipc_channels[channel].event_mask;
  const uint32_t fired      = sta & configured & k_ra_ipc_internal_event_full_mask;

  uint32_t message = 0U;
  if ((fired & k_ra_ipc_event_msg_ready) != 0U) {
    /* HUM Ch 3.2.13 "IPC0RXD0" p 216*/
    message = reg->RXD;
  }

  /* Decoded per-IRQ-line callbacks. */
  internal_ra_ipc_dispatch_irq_lines(channel, fired);

  /* Single shared callback. */
  const ra_ipc_event_fn_t fn  = s_ipc_callback;
  void* const             ctx = s_ipc_context;
  if ((fn != nullptr) && (fired != 0U)) {
    fn(ctx, channel, fired, message);
  }

  if (fired != 0U) {
    const uint32_t clr = internal_ra_ipc_event_to_clr(fired);
    if (clr != 0U) {
      /* HUM Ch 3.2.14 "IPC0CLR0" p 216*/
      reg->CLR = clr;
    }
  }
}

/**
 * @brief ISR trampoline -- dispatches both channels in one IPC unit.
 *
 * @details
 * The RA8D2 collapses both FIFO channels in a unit into a single ELC
 * event (HUM Ch 18 + bsp_elc.h). Inside the ISR we read STA on both
 * channels in the unit and call the regular dispatch path on each.
 *
 * @param[in] ctx See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static void internal_ra_ipc_isr(void* ctx)
{
  const uint8_t unit = (uint8_t)(uintptr_t)ctx;
  if ((uint16_t)unit >= (uint16_t)k_ra_ipc_nmi_unit_count) {
    return; /* GCOVR_EXCL_LINE */
  }
  /* HUM Ch 3.1 "Overview" p 204 -- channels 0/1 belong to IPC0
   * (unit 0); channels 2/3 belong to IPC1 (unit 1). */
  const uint8_t base = (uint8_t)((uint8_t)unit * 2U);
  ra_ipc_dispatch(base);
  ra_ipc_dispatch((uint8_t)(base + 1U));
}

[[nodiscard]] ra_err_t ra_ipc_install_isr(uint8_t unit, uint8_t priority)
{
  if ((uint16_t)unit >= (uint16_t)k_ra_ipc_nmi_unit_count) {
    return k_ra_err_invalid_arg;
  }
  if (s_ipc_isr_state[unit].installed) {
    return k_ra_err_exists;
  }
  const uint16_t event_raw = internal_ra_ipc_unit_to_event(unit);
  /* The IPC IRQ event values (0x05B / 0x05C, HUM Ch 18) are not yet
   * enumerated in ``ra_elc_event_t``; cast through ``uint16_t`` is
   * value-preserving since the enum's underlying type is ``uint16_t``.
   * The analyzer's enumerator-list check is suppressed for this
   * boundary cross only. */
  /* NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange) */
  const ra_elc_event_t event = (ra_elc_event_t)event_raw;
  /* HUM Ch 18 "Event Link Controller" event-list -- IPC IRQs route
   * through the ICU like every other peripheral; ra_isr_register
   * picks an IELSR slot, writes the event, sets priority and enables
   * the NVIC line. */
  const ra_err_t err =
    ra_isr_register(event, internal_ra_ipc_isr, (void*)(uintptr_t)unit, priority, nullptr);
  if (err != k_ra_ok) {
    return err;
  }
  s_ipc_isr_state[unit].installed = true;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_ipc_uninstall_isr(uint8_t unit)
{
  if ((uint16_t)unit >= (uint16_t)k_ra_ipc_nmi_unit_count) {
    return k_ra_err_invalid_arg;
  }
  if (!s_ipc_isr_state[unit].installed) {
    return k_ra_err_not_found;
  }
  const uint16_t event_raw = internal_ra_ipc_unit_to_event(unit);
  /* See ra_ipc_install_isr() for the rationale on this NOLINT. */
  /* NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange) */
  const ra_elc_event_t event = (ra_elc_event_t)event_raw;
  const ra_err_t       err   = ra_isr_unregister(event);
  if (err != k_ra_ok) {
    return err; /* GCOVR_EXCL_LINE */
  }
  s_ipc_isr_state[unit].installed = false;
  return k_ra_ok;
}

/* =============================================================================
 * Shared-memory ring-buffer protocol primitives
 * =============================================================================
 */

/**
 * @brief Validate the scalar fields of an ``ra_ipc_ring_t`` descriptor.
 *
 * @param[in] ring Caller-supplied, null-checked ring descriptor.
 * @return ``k_ra_ok`` if every field is in range, else
 *         ``k_ra_err_invalid_arg``.
 *
 * @pre ring != nullptr.
 * @pre ring->slots / head / tail are non-null.
 * @post No side effects.
 *
 * @details See implementation.
 * @retval k_ra_ok Operation succeeded.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static ra_err_t internal_ra_ipc_ring_validate(const ra_ipc_ring_t* ring)
{
  if (ring->capacity == 0U) {
    return k_ra_err_invalid_arg;
  }
  if ((ring->capacity & (ring->capacity - 1U)) != 0U) {
    /* Power-of-two capacity keeps the modulo cheap and unambiguous. */
    return k_ra_err_invalid_arg;
  }
  if ((uint16_t)ring->channel >= (uint16_t)k_ra_ipc_channel_count) {
    return k_ra_err_invalid_arg;
  }
  if ((uint16_t)ring->sem_id >= (uint16_t)k_ra_ipc_sem_count) {
    return k_ra_err_invalid_arg;
  }
  if ((uint16_t)ring->notify_id >= (uint16_t)k_ra_ipc_irq_event_count) {
    return k_ra_err_invalid_arg;
  }
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_ipc_ring_init(ra_ipc_ring_t* ring)
{
  RA_CHECK_NULL_PTR(ring, s_tag, "ring must not be nullptr");
  RA_CHECK_NULL_PTR(ring->slots, s_tag, "ring slots must not be nullptr");
  RA_CHECK_NULL_PTR(ring->head, s_tag, "ring head must not be nullptr");
  RA_CHECK_NULL_PTR(ring->tail, s_tag, "ring tail must not be nullptr");
  const ra_err_t err = internal_ra_ipc_ring_validate(ring);
  if (err != k_ra_ok) {
    return err;
  }
  *ring->head = 0U;
  *ring->tail = 0U;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_ipc_ring_produce(ra_ipc_ring_t* ring, uint32_t payload)
{
  RA_CHECK_NULL_PTR(ring, s_tag, "ring must not be nullptr");
  ra_err_t err = ra_ipc_sem_try_take(ring->sem_id);
  if (err != k_ra_ok) {
    return err;
  }
  const uint32_t head = *ring->head;
  const uint32_t tail = *ring->tail;
  if ((head - tail) >= ring->capacity) {
    /* Ring full -- release the semaphore before reporting busy so a
     * later consumer can acquire and drain. */
    (void)ra_ipc_sem_release(ring->sem_id);
    return k_ra_err_busy;
  }
  ring->slots[head & (ring->capacity - 1U)] = payload;
  *ring->head                               = head + 1U;
  err                                       = ra_ipc_sem_release(ring->sem_id);
  if (err != k_ra_ok) {
    return err; /* GCOVR_EXCL_LINE -- ring->sem_id already validated */
  }
  /* Wake the consumer through the configured IRQ event line. */
  return ra_ipc_send_event(ring->channel, ring->notify_id);
}

[[nodiscard]] ra_err_t ra_ipc_ring_consume(ra_ipc_ring_t* ring, uint32_t* out_payload)
{
  RA_CHECK_NULL_PTR(ring, s_tag, "ring must not be nullptr");
  RA_CHECK_NULL_PTR(out_payload, s_tag, "out_payload must not be nullptr");
  ra_err_t err = ra_ipc_sem_try_take(ring->sem_id);
  if (err != k_ra_ok) {
    return err;
  }
  const uint32_t head = *ring->head;
  const uint32_t tail = *ring->tail;
  if (head == tail) {
    (void)ra_ipc_sem_release(ring->sem_id);
    return k_ra_err_no_data;
  }
  *out_payload = ring->slots[tail & (ring->capacity - 1U)];
  *ring->tail  = tail + 1U;
  return ra_ipc_sem_release(ring->sem_id);
}

[[nodiscard]] ra_err_t ra_ipc_ring_is_empty(const ra_ipc_ring_t* ring, bool* out_empty)
{
  RA_CHECK_NULL_PTR(ring, s_tag, "ring must not be nullptr");
  RA_CHECK_NULL_PTR(out_empty, s_tag, "out_empty must not be nullptr");
  *out_empty = (*ring->head == *ring->tail);
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_ipc_ring_is_full(const ra_ipc_ring_t* ring, bool* out_full)
{
  RA_CHECK_NULL_PTR(ring, s_tag, "ring must not be nullptr");
  RA_CHECK_NULL_PTR(out_full, s_tag, "out_full must not be nullptr");
  *out_full = ((*ring->head - *ring->tail) >= ring->capacity);
  return k_ra_ok;
}
