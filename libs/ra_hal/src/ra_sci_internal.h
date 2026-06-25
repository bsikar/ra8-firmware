/**
 * @file ra_sci_internal.h
 * @brief src/-local shared surface for the ra_sci driver TUs.
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * The SCI_B driver is split across two translation units that share a
 * single per-channel dispatch table: ``ra_sci.c`` (configuration,
 * polling, attach, runtime reconfigure, async byte-stream) defines the
 * table, while ``ra_sci_dma_isr.c`` (DMA TX/RX descriptor build + ISR
 * dispatch) consumes it. This private header carries the channel-count
 * enum, the ``ra_sci_state_t`` layout, and an ``extern`` declaration of
 * the shared ``s_sci_state`` array so both TUs agree on the storage.
 *
 * Not a public API: only the ``ra_sci`` source files include this.
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

#include "ra_sci.h"

/**
 * @enum ra_sci_limits_inner_t
 * @brief File-local bounds and channel-table sizing.
 *
 * @details
 * Shared between the two ra_sci TUs so the channel index validation and
 * the ``s_sci_state`` array sizing stay in lock-step.
 *
 * @invariant ``k_ra_sci_channel_count_val`` ==
 *            ``k_ra_sci_channel_max_index`` + 1.
 *
 * @see s_sci_state
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra_sci_channel_max_index = 9U,  /**< SCI0..SCI9. */
  k_ra_sci_channel_count_val = 10U, /**< Total channels tracked. */
} ra_sci_limits_inner_t;

/**
 * @struct ra_sci_state_t
 * @brief Per-channel dispatch state.
 *
 * @details
 * Holds both the user-attached callback and the byte-stream state that
 * backs ``ra_sci_write`` / ``ra_sci_read``. The byte-stream fields
 * (``tx_buf`` / ``tx_len`` / ``tx_idx`` / ``rx_buf`` / ``rx_len`` /
 * ``rx_idx``) describe the in-flight async transfer: ``tx_idx`` and
 * ``rx_idx`` advance from 0 toward the matching ``*_len`` as TXI / RXI
 * fire. A direction is "idle" when its ``*_len`` is zero; that is the
 * sentinel checked by ``ra_sci_abort`` and ``ra_sci_read_stop``.
 *
 * @invariant ``tx_idx <= tx_len`` and ``rx_idx <= rx_len`` at all times.
 *
 * @see s_sci_state
 * @since 0.1.0
 */
typedef struct {
  ra_sci_rx_fn_t rx_fn;       /**< Attached RX handler, NULL if none. */
  void*          rx_ctx;      /**< RX handler context. */
  ra_sci_tx_fn_t tx_fn;       /**< Attached TX handler, NULL if none. */
  void*          tx_ctx;      /**< TX handler context. */
  bool           initialized; /**< True after ra_sci_init. */
  /* Async TX state (ra_sci_write). */
  const uint8_t* tx_buf; /**< Source buffer, NULL when idle. */
  uint32_t       tx_len; /**< Total bytes requested. 0 = idle. */
  uint32_t       tx_idx; /**< Next byte index to push. */
  /* Async RX state (ra_sci_read). */
  uint8_t* rx_buf; /**< Destination buffer, NULL when idle. */
  uint32_t rx_len; /**< Total bytes requested. 0 = idle. */
  uint32_t rx_idx; /**< Next byte index to write. */
} ra_sci_state_t;

/**
 * @var s_sci_state
 * @brief Per-channel allocation + dispatch table (defined in ra_sci.c).
 *
 * @details
 * The canonical storage lives in ``ra_sci.c``; ``ra_sci_dma_isr.c``
 * references it through this ``extern`` declaration so the ISR dispatch
 * path sees the same in-flight async TX/RX state the configuration path
 * mutates.
 *
 * @note Not thread-safe; mutated under IRQ-masked init and from the
 *       TXI/RXI ISR dispatch path.
 * @warning Do not redefine; the single definition is owned by ra_sci.c.
 * @since 0.1.0
 */
extern ra_sci_state_t s_sci_state[k_ra_sci_channel_count_val];

#ifdef __cplusplus
}
#endif
