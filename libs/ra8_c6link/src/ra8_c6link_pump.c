/**
 * @file ra8_c6link_pump.c
 * @brief The transaction loop: handshake, clock, classify, route.
 * @ingroup grp_net
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * This link is polled, not interrupt-driven. Every byte in either direction
 * moves inside a transaction that this loop starts, and a transaction only
 * happens when the co-processor has raised HANDSHAKE to say it has latched a
 * transmit buffer. That is upstream's `spi_transaction_task` reduced to what a
 * host with one thread and no queues needs:
 *
 *   1. wait, bounded, for HANDSHAKE;
 *   2. transmit either the one staged payload or an idle filler frame;
 *   3. clock ::k_ra8_c6link_frame_bytes full duplex;
 *   4. classify what came back and route it.
 *
 * A co-processor that has not armed the line ::k_ra8_c6link_hs_giveup times
 * running is not there. Spending the whole budget re-asking proves nothing and
 * turns "the harness is unplugged" into a minutes-long hang, so the loop stops
 * and the caller gets a timeout it can report.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_c6link.h"
#include "ra8_c6link_internal.h"

/**
 * @brief Wait, bounded, for the co-processor to arm HANDSHAKE.
 * @details The co-processor only latches its transmit buffer once it has
 *        raised the line, so a transaction started before that clocks nothing
 *        useful in either direction.
 * @param[in,out] link Open handle; must be non-null.
 * @return true when HANDSHAKE read active within the budget.
 * @retval true The co-processor is ready for a transaction.
 * @retval false ::k_ra8_c6link_hs_wait_ms elapsed with the line inactive.
 * @pre The transport seam is bound.
 * @pre HANDSHAKE is configured as an input by the backend.
 * @post No link state is modified.
 * @post At most ::k_ra8_c6link_hs_wait_ms elapsed.
 * @note The loop is bounded by ::k_ra8_c6link_hs_wait_ms (NASA Rule 2).
 * @since 0.1.0
 */
RA8_INTERNAL static bool ra8_c6link_pump_handshake(ra8_c6link_t* link)
{
  for (uint16_t waited = 0U; waited < (uint16_t)k_ra8_c6link_hs_wait_ms;
       waited          = (uint16_t)(waited + (uint16_t)k_ra8_c6link_hs_poll_ms)) {
    if (link->transport.handshake_active(link->transport.ctx)) {
      return true;
    }
    link->transport.delay_ms(link->transport.ctx, (uint16_t)k_ra8_c6link_hs_poll_ms);
  }
  return false;
}

/**
 * @brief Account for one classified frame and route the real ones.
 * @details Classifies first and routes second, so a frame that failed its
 *        integrity check reaches no consumer at all.
 * @param[in,out] link Open handle; must be non-null.
 * @param[in,out] stats Counters for the running pump; must be non-null.
 * @return true when the outstanding wait was satisfied.
 * @retval true The awaited answer arrived; the pump should stop.
 * @retval false Keep clocking.
 * @pre The transfer has completed and the receive transaction is stable.
 * @pre @p stats is the caller's live counter block.
 * @post Exactly one class counter in @p stats advanced.
 * @post A payload reached a consumer only if its checksum verified.
 * @note Ordering matches upstream's `process_spi_rx_buf()`.
 * @since 0.1.0
 */
RA8_INTERNAL static bool ra8_c6link_pump_receive(ra8_c6link_t* link, ra8_c6link_stats_t* stats)
{
  ra8_c6link_rx_view_t           view = {};
  const ra8_c6link_frame_class_t cls  = ra8_c6link_priv_frame_classify(link->rx, &view);

  switch (cls) {
    case k_ra8_c6link_frame_idle:
      stats->idle++;
      return false;
    case k_ra8_c6link_frame_malformed:
      stats->malformed++;
      return false;
    case k_ra8_c6link_frame_bad_checksum:
      stats->bad_checksum++;
      return false;
    case k_ra8_c6link_frame_data:
    default:
      break;
  }

  stats->data++;
  return ra8_c6link_priv_dispatch(link, &view);
}

RA8_PRIV ra8_err_t ra8_c6link_priv_pump(ra8_c6link_t*       link,
                                        uint16_t            max_transactions,
                                        ra8_c6link_stats_t* stats)
{
  if ((link == nullptr) || (stats == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  link->stats       = stats;
  uint16_t  misses  = 0U;
  ra8_err_t verdict = k_ra8_ok;

  for (uint16_t i = 0U; i < max_transactions; i++) {
    if (!ra8_c6link_pump_handshake(link)) {
      stats->hs_timeouts++;
      misses++;
      if (misses >= (uint16_t)k_ra8_c6link_hs_giveup) {
        break;
      }
      continue;
    }
    misses = 0U;

    if (link->tx_len != 0U) {
      ra8_c6link_priv_frame_seal(link->tx, link->tx_if, 0U, link->tx_len);
      link->tx_len = 0U;
    } else {
      ra8_c6link_priv_frame_filler(link->tx);
    }

    const ra8_err_t moved = link->transport.transfer(link->transport.ctx,
                                                     link->tx,
                                                     link->rx,
                                                     (uint16_t)k_ra8_c6link_frame_bytes);
    if (moved != k_ra8_ok) {
      verdict = k_ra8_err_spi_error;
      break;
    }
    stats->transfers++;

    if (ra8_c6link_pump_receive(link, stats)) {
      break;
    }
    link->transport.delay_ms(link->transport.ctx, (uint16_t)k_ra8_c6link_gap_ms);
  }

  link->stats = nullptr;
  if (verdict != k_ra8_ok) {
    return verdict;
  }
  return (stats->transfers == 0U) ? k_ra8_err_hw_timeout : k_ra8_ok;
}
