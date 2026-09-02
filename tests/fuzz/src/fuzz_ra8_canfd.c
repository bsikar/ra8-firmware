/**
 * @file fuzz_ra8_canfd.c
 * @brief libFuzzer harness for ra8_canfd RX frame parser
 *
 * @details
 * Stages arbitrary fuzz bytes directly into the RAM-backed CFDRF[0]
 * register block (the values the silicon RX engine would have
 * latched), then calls ::ra8_canfd_receive to exercise the ID / DLC /
 * FD-status decoder. Any out-of-bounds read, integer UB, or
 * descriptor overflow triggered by the decode path will be reported
 * by ASan / UBSan.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>

#include "fuzz_entry.h"
#include "ra8_canfd.h"
#include "ra8_canfd_regs.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"

enum : uint32_t {
  k_fuzz_canfd_max_input = 1024U, /**< Fuzz CANFD maximum input.      */
  k_fuzz_canfd_header    = 12U,   /**< 3 * uint32 = ID + PTR + FDSTS. */
  k_fuzz_canfd_channel   = 0U,    /**< Fuzz CANFD channel.            */
};

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
  if (size < (size_t)k_fuzz_canfd_header || size > (size_t)k_fuzz_canfd_max_input) {
    return 0;
  }
  ra8_fake_mmap_reset();
  if (ra8_canfd_init((uint8_t)k_fuzz_canfd_channel) != k_ra8_ok) {
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

  /* Stage the RAM-backed RX FIFO 0 block directly -- the values the
   * silicon RX engine would have latched after a frame landed. On the
   * host build these registers are plain mmap'd RAM, so the harness
   * plays the FIFO engine's role and the production receive path runs
   * its real register sequence against the staged block. */
  volatile r_canfd_t* reg = ra8_canfd((uint8_t)k_fuzz_canfd_channel);
  if (reg == nullptr) {
    return 0;
  }
  reg->CFDRF[0].ID    = id_word;
  reg->CFDRF[0].PTR   = ptr_word;
  reg->CFDRF[0].FDSTS = fdsts_word;
  uint32_t copy_len   = payload_len;
  if (copy_len > (uint32_t)k_ra8_canfd_data_bytes_max) {
    copy_len = (uint32_t)k_ra8_canfd_data_bytes_max;
  }
  for (uint32_t b = 0U; b < copy_len; b++) {
    reg->CFDRF[0].DF[b] = payload[b];
  }
  reg->CFDRFSTS[0] &= ~(uint32_t)k_ra8_rfsts_bit_empty;

  ra8_canfd_frame_t out = {};
  (void)ra8_canfd_receive((uint8_t)k_fuzz_canfd_channel, &out);
  return 0;
}
