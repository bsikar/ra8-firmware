/**
 * @file ra8_sci_internal.h
 * @brief src/-local shared surface for the ra8_sci driver TUs.
 * @ingroup grp_hal_comms
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * The SCI_B driver is split across two translation units that share a
 * single per-channel dispatch table: ``ra8_sci.c`` (configuration,
 * polling, attach, runtime reconfigure, async byte-stream) defines the
 * table, while ``ra8_sci_dma_isr.c`` (DMA TX/RX descriptor build + ISR
 * dispatch) consumes it. This private header carries the channel-count
 * enum, the ``ra8_sci_state_t`` layout, and an ``extern`` declaration of
 * the shared ``s_sci_state`` array so both TUs agree on the storage.
 *
 * Not a public API: only the ``ra8_sci`` source files include this.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_sci.h"

/**
 * @enum ra8_sci_limits_inner_t
 * @brief File-local bounds and channel-table sizing.
 *
 * @details
 * Shared between the two ra8_sci TUs so the channel index validation and
 * the ``s_sci_state`` array sizing stay in lock-step.
 *
 * @invariant ``k_ra8_sci_channel_count_val`` ==
 *            ``k_ra8_sci_channel_max_index`` + 1.
 *
 * @see s_sci_state
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_sci_channel_max_index = 9U,  /**< SCI0..SCI9.             */
  k_ra8_sci_channel_count_val = 10U, /**< Total channels tracked. */
} ra8_sci_limits_inner_t;

/**
 * @struct ra8_sci_state_t
 * @brief Per-channel dispatch state.
 *
 * @details
 * Holds both the user-attached callback and the byte-stream state that
 * backs ``ra8_sci_write`` / ``ra8_sci_read``. The byte-stream fields
 * (``tx_buf`` / ``tx_len`` / ``tx_idx`` / ``rx_buf`` / ``rx_len`` /
 * ``rx_idx``) describe the in-flight async transfer: ``tx_idx`` and
 * ``rx_idx`` advance from 0 toward the matching ``*_len`` as TXI / RXI
 * fire. A direction is "idle" when its ``*_len`` is zero; that is the
 * sentinel checked by ``ra8_sci_abort`` and ``ra8_sci_read_stop``.
 *
 * @invariant ``tx_idx <= tx_len`` and ``rx_idx <= rx_len`` at all times.
 *
 * @see s_sci_state
 * @since 0.1.0
 */
typedef struct {
  ra8_sci_rx_fn_t rx_fn;       /**< Attached RX handler, NULL if none. */
  void*           rx_ctx;      /**< RX handler context.                */
  ra8_sci_tx_fn_t tx_fn;       /**< Attached TX handler, NULL if none. */
  void*           tx_ctx;      /**< TX handler context.                */
  bool            initialized; /**< True after ra8_sci_init.           */
  /* Async TX state (ra8_sci_write). */
  const uint8_t* tx_buf; /**< Source buffer, NULL when idle.   */
  uint32_t       tx_len; /**< Total bytes requested. 0 = idle. */
  uint32_t       tx_idx; /**< Next byte index to push.         */
  /* Async RX state (ra8_sci_read). */
  uint8_t* rx_buf; /**< Destination buffer, NULL when idle. */
  uint32_t rx_len; /**< Total bytes requested. 0 = idle.    */
  uint32_t rx_idx; /**< Next byte index to write.           */
} ra8_sci_state_t;

/**
 * @var s_sci_state
 * @brief Per-channel allocation + dispatch table (defined in ra8_sci.c).
 *
 * @details
 * The canonical storage lives in ``ra8_sci.c``; ``ra8_sci_dma_isr.c``
 * references it through this ``extern`` declaration so the ISR dispatch
 * path sees the same in-flight async TX/RX state the configuration path
 * mutates.
 *
 * @note Not thread-safe by itself. Its async TX/RX fields are published and
 *       torn down by the mainline runtime APIs (``ra8_sci_write`` / ``read`` /
 *       ``abort`` / ``read_stop`` / ``deinit`` / the ``attach_*_handler`` pair)
 *       from inside an ``ra8_register_guard`` PRIMASK critical section, and are
 *       also mutated from the TXI/RXI ISR dispatch path in ``ra8_sci_dma_isr.c``.
 *       The critical section (``cpsid i`` + a ``"memory"`` clobber) both
 *       serialises the read-modify-write against a same-core ISR and prevents
 *       the compiler from reordering the descriptor publish past the interrupt
 *       enable (#176 / T1-02). ``ra8_sci_init`` publishes the initial zeroed
 *       state during single-threaded bring-up, before the channel IRQ is armed.
 * @warning Do not redefine; the single definition is owned by ra8_sci.c.
 * @since 0.1.0
 */
extern ra8_sci_state_t s_sci_state[k_ra8_sci_channel_count_val];

#ifdef __cplusplus
}
#endif
