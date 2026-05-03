/**
 * @file fuzz_ra_canfd.c
 * @brief libFuzzer harness for ra_canfd RX frame parser
 *
 * @details
 * Drives arbitrary fuzz bytes into the simulator's CFDRF[0]
 * register block via ::ra_canfd_test_inject_frame, then calls
 * ::ra_canfd_receive to exercise the ID / DLC / FD-status decoder.
 * Any out-of-bounds read, integer UB, or descriptor overflow
 * triggered by the decode path will be reported by ASan / UBSan.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>

#include "ra_canfd.h"
#include "ra_err.h"
#include "ra_sim_mmap.h"

enum : uint32_t {
  k_fuzz_canfd_max_input = 1024U,
  k_fuzz_canfd_header    = 12U, /* 3 * uint32 = ID + PTR + FDSTS. */
  k_fuzz_canfd_channel   = 0U,
};

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
  if (size < (size_t)k_fuzz_canfd_header || size > (size_t)k_fuzz_canfd_max_input) {
    return 0;
  }
  ra_sim_mmap_reset();
  if (ra_canfd_init((uint8_t)k_fuzz_canfd_channel) != k_ra_ok) {
    return 0;
  }

  /* First 12 bytes seed ID / PTR / FDSTS in little-endian. */
  uint32_t id_word    = 0U;
  uint32_t ptr_word   = 0U;
  uint32_t fdsts_word = 0U;
  for (uint8_t i = 0U; i < 4U; i++) {
    id_word |= ((uint32_t)data[i]) << (8U * i);
    ptr_word |= ((uint32_t)data[i + 4U]) << (8U * i);
    fdsts_word |= ((uint32_t)data[i + 8U]) << (8U * i);
  }
  const uint8_t* payload     = data + (size_t)k_fuzz_canfd_header;
  const uint32_t payload_len = (uint32_t)(size - (size_t)k_fuzz_canfd_header);

  if (ra_canfd_test_inject_frame((uint8_t)k_fuzz_canfd_channel,
                                 id_word,
                                 ptr_word,
                                 fdsts_word,
                                 payload,
                                 payload_len) != k_ra_ok) {
    return 0;
  }
  ra_canfd_frame_t out = {};
  (void)ra_canfd_receive((uint8_t)k_fuzz_canfd_channel, &out);
  return 0;
}
