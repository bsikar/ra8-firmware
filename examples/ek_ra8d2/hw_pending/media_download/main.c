/**
 * @file main.c
 * @brief Honest placeholder for pending e-reader media-download board wiring
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details The reusable transfer coordinator is model-tested, but this board
 * application still needs an opened C6 transport plus transactional storage
 * and SHA-256 adapters. It deliberately sends no RPC until those mechanisms
 * exist and mixed-image HIL proves them.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_c6link_mdl_transfer.h"
#include "ra8_log.h"

/**
 * @brief Report the deliberately hardware-pending integration state
 * @return Process status.
 * @retval 0 No unsupported RPC operation was attempted.
 * @pre The board-selected ::ra8_log backend is available for this fixture.
 * @pre Hardware support is not inferred from compiling this application.
 * @post The pending-integration state is emitted through ::ra8_log.
 * @post No c6link request has been sent.
 * @post No temporary or committed storage object has been created.
 * @note Single-threaded compile fixture.
 * @since 0.1.0
 */
int main(void)
{
  static_assert(sizeof(ra8_mdl_transfer_result_t) > k_ra8_mdl_sha256_bytes);
  ra8_log_init();
  ra8_log_warn("media_download", "board transport/storage/hash integration pending");
  return 0;
}
