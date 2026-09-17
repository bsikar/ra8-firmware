/**
 * @file ra8_c6link_transport.h
 * @brief The one seam between `ra8_c6link` and the wire that reaches the C6.
 * @ingroup grp_net
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Everything `ra8_c6link` knows about hardware is these three function
 * pointers. The facade above them frames, checksums, encodes protobuf and
 * correlates responses; none of that changes if the C6 is reached over
 * full-duplex SPI today and over something else tomorrow, so none of it is
 * allowed to name a peripheral.
 *
 * Two implementations exist and are interchangeable (Liskov):
 *
 *   - `port/esp-hosted/` binds the seam to the ported esp-hosted OS-abstraction
 *     vtable, which is what drives the SCI Simple-SPI channel on the board;
 *   - `tests/wireless/src/test_ra8_c6link.c` binds it to a co-processor model that speaks
 *     the real wire protocol back at the facade, which is how every layer above
 *     this file is proven with no board attached.
 *
 * @par Why the seam is exactly three rows
 * A transaction on this link is: wait until the co-processor says it is armed,
 * then clock a fixed-size buffer out and another one in, then let something
 * else run. There is no read-only or write-only direction to express -- the bus
 * is full duplex and every transaction moves ::k_ra8_c6link_frame_bytes in both
 * directions whether or not either side had anything to say.
 *
 * `DATA_READY` is deliberately absent. The board routes it and the port reads
 * it, but no first-party evidence yet describes how the co-processor drives it
 * under load, and a facade that gated transactions on an unproven signal would
 * be guessing. Until #492 measures it, this link discovers pending receive data
 * by clocking a transaction, which is what the bench-proven bring-up did.
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

#include "ra8_err.h"

/**
 * @struct ra8_c6link_transport
 * @brief Function-pointer seam a backend fills so the facade can reach the C6.
 *
 * @details
 * Caller-allocated and copied into the link handle by ::ra8_c6link_open, so the
 * structure itself need not outlive the open call -- but everything it points
 * at must outlive the link. Every row is mandatory: a partially-filled seam is
 * rejected at open rather than discovered as a null call at the first
 * transaction.
 *
 * NASA Power of 10 Rule 9 permits function pointers precisely for this:
 * Dependency Inversion, so the protocol layers are host-testable.
 *
 * @invariant `transfer`, `handshake_active` and `delay_ms` are all non-null
 *            once ::ra8_c6link_open has accepted the seam.
 * @invariant `ctx` is passed back unmodified to every row and is never
 *            dereferenced by the facade.
 *
 * @par Example:
 * @code
 * ra8_c6link_transport_t seam = {
 *   .transfer         = my_spi_transfer,
 *   .handshake_active = my_handshake_read,
 *   .delay_ms         = my_delay,
 *   .ctx              = &my_bus,
 * };
 * @endcode
 *
 * @see ra8_c6link_open
 * @since 0.1.0
 */
typedef struct ra8_c6link_transport {
  /**
   * @brief Clock one full-duplex transaction of exactly @p len bytes.
   * @details Must transmit every byte of `tx` and store every received byte in
   *          `rx`; both buffers are ::k_ra8_c6link_frame_bytes long and neither
   *          is ever null. Returning anything but `k_ra8_ok` aborts the pump
   *          and is reported to the caller as a bus fault.
   */
  ra8_err_t (*transfer)(void* ctx, const uint8_t* tx, uint8_t* rx, uint16_t len);

  /**
   * @brief Report whether the co-processor is armed for a transaction.
   * @details The HANDSHAKE line, sampled at its active level. The facade polls
   *          this and never starts a transaction while it reads false, because
   *          the co-processor only latches its transmit buffer once it has
   *          raised the line.
   */
  bool (*handshake_active)(void* ctx);

  /**
   * @brief Yield for approximately @p ms milliseconds.
   * @details Called between handshake samples and between transactions. May
   *          block; on the target it is the RTOS sleep, in host tests it
   *          advances the model's clock and returns immediately.
   */
  void (*delay_ms)(void* ctx, uint16_t ms);

  /** @brief Opaque backend context, handed back to every row above. */
  void* ctx;
} ra8_c6link_transport_t;

#ifdef __cplusplus
}
#endif
