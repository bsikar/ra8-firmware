/**
 * @file fuzz_unarch_tar.c
 * @brief libFuzzer harness for the clean-room streaming tar walker.
 *
 * @details
 * `.cbt` comics and unwrapped `.tar.gz` / `.tar.xz` content are untrusted
 * initial-access input. This harness treats the fuzz input as a tar
 * archive: it opens it (`unarch_tar_open`, default decompression-limits
 * policy), walks every member (`unarch_tar_next` -- ustar / pax / GNU
 * header grammars, the hostile-byte surface), and extracts each file member
 * into a fixed buffer (`unarch_tar_read`). The walker must reject every
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
#include "unarch_io.h"
#include "unarch_tar.h"

/**
 * @enum unarch_tar_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_unarch_tar_u_20 = 20, /**< Log2 of the fuzz input-size cap (1 MiB). */
} unarch_tar_uint8_const_t;

/**
 * @enum unarch_tar_uint16_const_t
 * @brief Named uint16_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint16_t {
  k_unarch_tar_n_4096   = 4096U, /**< Hard cap on archive members walked (NASA P10 Rule 2 bound). */
  k_unarch_tar_name_cap = 512,   /**< Extracted-entry name buffer capacity.                       */
} unarch_tar_uint16_const_t;

/** @brief Member extraction capacity. */
typedef enum : uint32_t {
  k_unarch_tar_out_cap = 65536U, /**< Maximum extracted member bytes. */
} unarch_tar_uint32_const_t;

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
  static uint8_t s_out[k_unarch_tar_out_cap];
  static char    s_name[k_unarch_tar_name_cap];
  if ((size == 0U) || (size > (1U << k_unarch_tar_u_20))) {
    return 0;
  }
  unarch_mem_t mem = {.base = data, .len = size};
  unarch_tar_t t   = {};
  if (unarch_tar_open(&t, unarch_mem_read, &mem, (uint64_t)size, nullptr) != k_ra8_ok) {
    return 0;
  }
  uint64_t off  = 0U;
  bool     walk = true;
  for (uint32_t n = 0U; (n < k_unarch_tar_n_4096) && walk; ++n) { /* bound: hard member cap */
    unarch_tar_entry_t ent = {};
    if (unarch_tar_next(&t, off, s_name, (uint16_t)sizeof(s_name), &ent) != k_ra8_ok) {
      walk = false;
    } else {
      if ((ent.is_file != 0U) && (ent.size <= (uint64_t)sizeof(s_out))) {
        size_t got = 0U;
        (void)unarch_tar_read(&t, &ent, s_out, sizeof(s_out), &got);
      }
      if (ent.next_off <= off) {
        walk = false;
      } else {
        off = ent.next_off;
      }
    }
  }
  return 0;
}
