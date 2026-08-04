/**
 * @file fuzz_ra8_unarch_xz.c
 * @brief libFuzzer harness for the bounded XZ decoder wrapper.
 *
 * @details
 * `.tar.xz` comics are untrusted SD-card content. This harness treats the
 * fuzz input as one XZ stream and decodes it whole through
 * `ra8_unarch_xz_unwrap` (session begin / windowed run / end, every pass
 * charged against the default decompression-limits policy) into a fixed
 * arena. The wrapper must reject every malformed / hostile stream without
 * crashing, over-reading, or looping unboundedly; ASan / UBSan diagnose any
 * out-of-bounds access or integer UB inside the wrapper and the vendored
 * xz-embedded decoder.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>

#include "fuzz_entry.h"
#include "ra8_err.h"
#include "ra8_unarch_io.h"
#include "ra8_unarch_xz.h"

/**
 * @enum unarch_xz_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_unarch_xz_u_20 = 20, /**< Log2 of the decompression arena size (1 MiB). */
} unarch_xz_uint8_const_t;

/**
 * @enum unarch_xz_uint16_const_t
 * @brief Named uint16_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint16_t {
  k_unarch_xz_u_1024      = 1024U, /**< Bytes per KiB, scaling the scratch arena. */
  k_unarch_xz_scratch_kib = 320U,  /**< Decoder scratch arena size, in KiB.       */
} unarch_xz_uint16_const_t;

/** @brief Decode destination arena (bounds every honest stream too). */
static uint8_t s_arena[1U << k_unarch_xz_u_20];
/** @brief XZ session scratch: 64 KiB state reserve + 256 KiB dict budget. */
static _Alignas(8) uint8_t s_scratch[k_unarch_xz_scratch_kib * k_unarch_xz_u_1024];

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
  if (size == 0U || size > (1U << k_unarch_xz_u_20)) {
    return 0;
  }
  ra8_unarch_mem_t mem = {.base = data, .len = size};
  size_t           got = 0U;
  (void)ra8_unarch_xz_unwrap(ra8_unarch_mem_read,
                             &mem,
                             (uint64_t)size,
                             s_arena,
                             sizeof(s_arena),
                             s_scratch,
                             (uint32_t)sizeof(s_scratch),
                             nullptr,
                             &got);
  return 0;
}
