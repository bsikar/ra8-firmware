/**
 * @file ra8_ipc_types.h
 * @brief Inter-Processor Communication (IPC) HAL driver -- public types
 * @ingroup grp_hal_system
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Public typed enums, structures, and callback typedefs for the RA8D2
 * IPC mailbox driver. Split out of ``ra8_ipc.h`` so each header stays
 * within the repository file-size budget; ``ra8_ipc.h`` re-includes this
 * header so existing consumers are unaffected.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_ipc_regs.h"

/* =============================================================================
 * Public types
 * =============================================================================
 */

/**
 * @enum ra8_ipc_event_t
 * @brief Bitmask reported to the user callback during dispatch.
 *
 * @details
 * Mirrors the IPCnSTAm bit layout from HUM Ch 3.2.10 p 214 in a form
 * suitable for the public API. Multiple flags can coexist in a single
 * dispatch -- e.g. RDY + IRQ0 if a peer wrote a message and signalled
 * an event in the same window.
 */
typedef enum : uint32_t {
  k_ra8_ipc_event_none      = 0x00000000U, /**< No event pending.            */
  k_ra8_ipc_event_irq0      = 0x00000001U, /**< Maskable IRQ event line 0.   */
  k_ra8_ipc_event_irq1      = 0x00000002U, /**< Maskable IRQ event line 1.   */
  k_ra8_ipc_event_irq2      = 0x00000004U, /**< Maskable IRQ event line 2.   */
  k_ra8_ipc_event_irq3      = 0x00000008U, /**< Maskable IRQ event line 3.   */
  k_ra8_ipc_event_irq4      = 0x00000010U, /**< Maskable IRQ event line 4.   */
  k_ra8_ipc_event_irq5      = 0x00000020U, /**< Maskable IRQ event line 5.   */
  k_ra8_ipc_event_irq6      = 0x00000040U, /**< Maskable IRQ event line 6.   */
  k_ra8_ipc_event_irq7      = 0x00000080U, /**< Maskable IRQ event line 7.   */
  k_ra8_ipc_event_irq_all   = 0x000000FFU, /**< IRQ7..IRQ0 union mask.       */
  k_ra8_ipc_event_msg_ready = 0x00010000U, /**< RDY: receive FIFO non-empty. */
  k_ra8_ipc_event_fifo_full = 0x00020000U, /**< FULL: send FIFO is full.     */
  k_ra8_ipc_event_err_empty = 0x01000000U, /**< RERR: read-while-empty.      */
  k_ra8_ipc_event_err_full  = 0x02000000U, /**< FERR: write-while-full.      */
} ra8_ipc_event_t;

/**
 * @enum ra8_ipc_irq_event_id_t
 * @brief Index of an IRQ event line within a channel (0..7).
 */
typedef enum : uint8_t {
  k_ra8_ipc_irq_event_0 = 0U, /**< RA8 ipc IRQ event 0. */
  k_ra8_ipc_irq_event_1 = 1U, /**< RA8 ipc IRQ event 1. */
  k_ra8_ipc_irq_event_2 = 2U, /**< RA8 ipc IRQ event 2. */
  k_ra8_ipc_irq_event_3 = 3U, /**< RA8 ipc IRQ event 3. */
  k_ra8_ipc_irq_event_4 = 4U, /**< RA8 ipc IRQ event 4. */
  k_ra8_ipc_irq_event_5 = 5U, /**< RA8 ipc IRQ event 5. */
  k_ra8_ipc_irq_event_6 = 6U, /**< RA8 ipc IRQ event 6. */
  k_ra8_ipc_irq_event_7 = 7U, /**< RA8 ipc IRQ event 7. */
} ra8_ipc_irq_event_id_t;

/**
 * @enum ra8_ipc_retry_limits_t
 * @brief Retry-loop bounds used by the bounded-wait helpers.
 *
 * @details
 * NASA Power of 10 Rule 2 demands all loops have statically-provable
 * upper bounds. ``ra8_ipc_send_message_retry`` and
 * ``ra8_ipc_recv_message_retry`` use these enums so the loop bound is
 * a compile-time constant, not a magic number.
 */
typedef enum : uint16_t {
  k_ra8_ipc_retry_default = 16U,   /**< Default poll bound for FIFO retry. */
  k_ra8_ipc_retry_max     = 1024U, /**< Hard cap accepted by the API.      */
  k_ra8_ipc_sem_take_max  = 1024U, /**< Hard cap on semaphore-take spins.  */
} ra8_ipc_retry_limits_t;

/**
 * @struct ra8_ipc_attr_t
 * @brief Snapshot of the security / privilege attribution for one
 *        IPC channel, decoded from IPCSAR / IPCPAR.
 *
 * @details
 * cppcheck cannot see tests/ so it flags every field as unused; each
 * member is written by ``ra8_ipc_get_attribution`` in
 * ``libs/ra8_hal/src/ra8_ipc.c`` and read by the unit tests.
 */
typedef struct {
  bool secure;     /**< true -> non-secure, false -> secure (IPCSAR.SAIPCIRn).       */
  bool privileged; /**< true -> unprivileged, false -> privileged (IPCPAR.PAIPCIRn). */
} ra8_ipc_attr_t;

/**
 * @struct ra8_ipc_config_t
 * @brief Per-channel configuration descriptor passed to
 *        ``ra8_ipc_init``.
 */
typedef struct {
  uint8_t  channel;      /**< Channel id 0..3.                        */
  bool     reset_fifo;   /**< true -> issue CLR.RST during init.      */
  bool     clear_status; /**< true -> clear all IRQ + error bits.     */
  uint32_t event_mask;   /**< Bitmask of events the user cares about. */
} ra8_ipc_config_t;

/**
 * @typedef ra8_ipc_event_fn_t
 * @brief IPC event callback signature.
 *
 * @param[in] ctx        Context pointer captured at attach time.
 * @param[in] channel    Channel id 0..3 that fired the event.
 * @param[in] event_mask Bitwise OR of ``ra8_ipc_event_t`` flags.
 * @param[in] message    Message word read from RXD when the event
 *                       set ``k_ra8_ipc_event_msg_ready``; 0 otherwise.
 */
typedef void (*ra8_ipc_event_fn_t)(void*    ctx,
                                   uint8_t  channel,
                                   uint32_t event_mask,
                                   uint32_t message);

/**
 * @typedef ra8_ipc_irq_fn_t
 * @brief Per-IRQ-event-line callback signature.
 *
 * @param[in] ctx     Context pointer captured at attach time.
 * @param[in] channel Channel id 0..3.
 * @param[in] event_id IRQ event id 0..7 that fired.
 */
typedef void (*ra8_ipc_irq_fn_t)(void* ctx, uint8_t channel, ra8_ipc_irq_event_id_t event_id);

/**
 * @typedef ra8_ipc_nmi_fn_t
 * @brief NMI dispatch callback signature.
 *
 * @param[in] ctx  Context pointer captured at attach time.
 * @param[in] unit NMI unit id 0..1 (k_ra8_ipc_unit_ipc0 / ipc1).
 */
typedef void (*ra8_ipc_nmi_fn_t)(void* ctx, uint8_t unit);

/**
 * @struct ra8_ipc_ring_t
 * @brief Shared-memory ring-buffer descriptor used by the producer /
 *        consumer protocol primitives.
 *
 * @details
 * The hardware FIFO carries 32-bit words only -- larger payloads are
 * placed in a shared SRAM region and the FIFO carries the producer
 * head index. The ring uses a single hardware semaphore (IPCSEMn) for
 * mutual exclusion of the head / tail update, and the chosen IPC
 * channel for cross-core notification.
 *
 * cppcheck cannot see the tests so it flags every field as unused.
 */
typedef struct {
  volatile uint32_t*     slots;     /**< Backing SRAM array (caller-owned).  */
  volatile uint32_t*     head;      /**< Producer write index (shared).      */
  volatile uint32_t*     tail;      /**< Consumer read index (shared).       */
  uint32_t               capacity;  /**< Number of 32-bit slots in @p slots. */
  uint8_t                channel;   /**< IPC channel used for notification.  */
  uint8_t                sem_id;    /**< IPCSEM index used for exclusion.    */
  ra8_ipc_irq_event_id_t notify_id; /**< IRQ event line used for notify.     */
} ra8_ipc_ring_t;
