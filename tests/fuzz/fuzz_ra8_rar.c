/**
 * @file fuzz_ra8_rar.c
 * @brief libFuzzer harness for the clean-room RAR walker + RAR5 decompressor.
 *
 * @details
 * `.cbr` comic archives are untrusted initial-access content. This harness treats
 * the fuzz input as a RAR container: it opens it (`ra8_rar_open`), walks every block
 * header (`ra8_rar_next`), and extracts each file member (`ra8_rar_extract`, which
 * routes STORE by copy and RAR5-compressed through `ra8_rar5_decompress`) into a
 * fixed buffer. The walker and decoder must reject every malformed / hostile input
 * without crashing, over-reading, or looping unboundedly. ASan / UBSan diagnose any
 * out-of-bounds access or integer UB.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "fuzz_entry.h"
#include "ra8_err.h"
#include "ra8_rar.h"
#include "ra8_rar5.h"

/**
 * @enum rar_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_rar_u_20 = 20,
} rar_uint8_const_t;

/**
 * @enum rar_uint16_const_t
 * @brief Named uint16_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint16_t {
  k_rar_n_4096  = 4096U,
  k_rar_val_512 = 512,
} rar_uint16_const_t;

/** @brief Flat backing served to the walker. */
typedef struct {
  const uint8_t* data; /**< Data. */
  size_t         size; /**< Size. */
} fz_src_t;

static size_t fz_read(void* ctx, uint64_t off, void* dst, size_t len)
{
  const fz_src_t* s = (const fz_src_t*)ctx;
  if (off >= (uint64_t)s->size) {
    return 0U;
  }
  const uint64_t avail = (uint64_t)s->size - off;
  const size_t   n     = (len > (size_t)avail) ? (size_t)avail : len;
  memcpy(dst, &s->data[off], n);
  return n;
}

static ra8_rar5_state_t s_state;
static uint8_t          s_out[1U << 16];
static char             s_name[k_rar_val_512];

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
  if (size == 0U || size > (1U << k_rar_u_20)) {
    return 0;
  }
  fz_src_t  src = {.data = data, .size = size};
  ra8_rar_t rar = {};
  if (ra8_rar_open(&rar, fz_read, &src, (uint64_t)size) != k_ra8_ok) {
    return 0;
  }
  uint64_t off = rar.first_off;
  for (uint32_t n = 0U; n < k_rar_n_4096; ++n) { /* bound: hard block cap */
    if (off >= rar.size) {
      break;
    }
    ra8_rar_entry_t ent = {};
    if (ra8_rar_next(&rar, off, s_name, (uint16_t)sizeof(s_name), &ent) != k_ra8_ok) {
      break;
    }
    if (ent.is_file != 0U && ent.is_dir == 0U && ent.unp_size <= (uint64_t)sizeof(s_out)) {
      size_t got = 0U;
      (void)ra8_rar_extract(&rar, &ent, s_out, sizeof(s_out), &s_state, &got);
    }
    if (ent.next_off <= off) {
      break;
    }
    off = ent.next_off;
  }
  return 0;
}
