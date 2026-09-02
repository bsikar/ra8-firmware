/**
 * @file main.c
 * @brief Cross-build fixture for the bounded C6 media RPC types
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details This fixture proves the generated protocol and coordinator types
 * cross-compile. It does not instantiate an unopened link or claim hardware
 * execution; protocol behavior is verified by the host-side C6 model.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_boot_entry.h"
#include "ra8_c6link_mdl.h"
#include "ra8_c6link_mdl_transfer.h"

/**
 * @brief Prove the public raw-transfer types are available to a target app
 * @pre The application links `ra8_c6link` and ESP-hosted generated codecs.
 * @pre No hardware validation is inferred from this function.
 * @post No c6link operation is attempted.
 * @post No network or storage state is modified.
 * @note Single-threaded compile fixture.
 * @since 0.1.0
 */
void main(void)
{
  static_assert(sizeof(ra8_mdl_chunk_t) >= k_ra8_mdl_chunk_data_max);
  static_assert(sizeof(ra8_mdl_transfer_result_t) > k_ra8_mdl_sha256_bytes);
}
