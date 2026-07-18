/**
 * @file fuzz_ra8_unarch_tar.c
 * @brief libFuzzer harness for the clean-room streaming tar walker.
 *
 * @details
 * `.cbt` comics and unwrapped `.tar.gz` / `.tar.xz` content are untrusted
 * initial-access input. This harness treats the fuzz input as a tar
 * archive: it opens it (`ra8_unarch_tar_open`, default decompression-limits
 * policy), walks every member (`ra8_unarch_tar_next` -- ustar / pax / GNU
 * header grammars, the hostile-byte surface), and extracts each file member
 * into a fixed buffer (`ra8_unarch_tar_read`). The walker must reject every
 * malformed / lying / flooding chain without crashing, over-reading, or
 * looping unboundedly; ASan / UBSan diagnose any out-of-bounds access or
 * integer UB.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>

#include "fuzz_entry.h"
#include "ra8_err.h"
#include "ra8_unarch_io.h"
#include "ra8_unarch_tar.h"

/** @brief Member extraction buffer. */
static uint8_t s_out[1U << 16];
/** @brief Member name buffer. */
static char s_name[512];

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
  if (size == 0U || size > (1U << 20)) {
    return 0;
  }
  ra8_unarch_mem_t mem = {.base = data, .len = size};
  ra8_unarch_tar_t t   = {};
  if (ra8_unarch_tar_open(&t, ra8_unarch_mem_read, &mem, (uint64_t)size, nullptr) != k_ra8_ok) {
    return 0;
  }
  uint64_t off = 0U;
  for (uint32_t n = 0U; n < 4096U; ++n) { /* bound: hard member cap */
    ra8_unarch_tar_entry_t ent = {};
    if (ra8_unarch_tar_next(&t, off, s_name, (uint16_t)sizeof(s_name), &ent) != k_ra8_ok) {
      break;
    }
    if (ent.is_file != 0U && ent.size <= (uint64_t)sizeof(s_out)) {
      size_t got = 0U;
      (void)ra8_unarch_tar_read(&t, &ent, s_out, sizeof(s_out), &got);
    }
    if (ent.next_off <= off) {
      break;
    }
    off = ent.next_off;
  }
  return 0;
}
