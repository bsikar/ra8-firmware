/**
 * @file fuzz_ra8_unarch_gzip.c
 * @brief libFuzzer harness for the clean-room gzip member decoder.
 *
 * @details
 * `.tar.gz` / `.cbt.gz` comics are untrusted SD-card content. This harness
 * treats the fuzz input as one gzip member and decodes it whole through
 * `ra8_unarch_gzip_unwrap` (RFC 1952 header parse, windowed tinfl DEFLATE
 * inflation charged against the default decompression-limits policy,
 * trailer CRC32 + ISIZE verification) into a fixed arena. The decoder must
 * reject every malformed / hostile member without crashing, over-reading,
 * or looping unboundedly; ASan / UBSan diagnose any out-of-bounds access or
 * integer UB inside the first-party container parser and the vendored miniz
 * DEFLATE core.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>

#include "fuzz_entry.h"
#include "ra8_err.h"
#include "ra8_unarch_gzip.h"
#include "ra8_unarch_io.h"

/**
 * @enum unarch_gzip_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_unarch_gzip_u_20 = 20,
} unarch_gzip_uint8_const_t;

/** @brief Decode destination arena (bounds every honest member too). */
static uint8_t s_arena[1U << k_unarch_gzip_u_20];

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
  if (size == 0U || size > (1U << k_unarch_gzip_u_20)) {
    return 0;
  }
  ra8_unarch_mem_t mem = {.base = data, .len = size};
  size_t           got = 0U;
  (void)ra8_unarch_gzip_unwrap(ra8_unarch_mem_read,
                               &mem,
                               (uint64_t)size,
                               s_arena,
                               sizeof(s_arena),
                               nullptr,
                               &got);
  return 0;
}
