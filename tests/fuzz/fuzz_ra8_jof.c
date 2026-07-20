/**
 * @file fuzz_ra8_jof.c
 * @brief libFuzzer harness for the JOF atlas reader (#231).
 *
 * @details
 * A tile atlas can arrive pre-baked inside a downloaded EPUB (the
 * `ra8_epub_tile_binder_add()` in-place path), which makes the atlas bytes
 * themselves attacker-controlled. This harness treats the raw fuzz input as
 * an atlas backing store: `ra8_jof_parse()` validates the structure,
 * then every tile of an accepted atlas is decoded through
 * `ra8_jof_read_tile()` into fixed buffers. An ASan / UBSan diagnostic
 * here is a real out-of-bounds access or integer overflow in the header /
 * footer / index validation or the raw/deflate tile decode. Every random
 * input is expected to fail the parse closed without crashing.
 *
 * Build via tests/fuzz/CMakeLists.txt with -DRA8_FUZZ=ON.
 * Run for a longer fuzz session via scripts/utils/run_fuzz.sh.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "fuzz_entry.h"
#include "ra8_err.h"
#include "ra8_jof.h"

/** @brief Harness sizing: page-back buffers for one worst-case tile. */
enum : uint32_t {
  k_fuzz_rta_max_input = 1U << 20,           /**< Cap fuzz input at 1 MiB. */
  k_fuzz_rta_cell      = 1U * 1024U * 1024U, /**< Tile page-back buffer.   */
  k_fuzz_rta_scratch   = 2U * 1024U * 1024U, /**< Stored-tile staging.     */
  k_fuzz_rta_max_tiles = 64U,                /**< Tiles decoded per input. */
};

/** @brief Tile page-back buffer. */
static uint8_t s_cell[k_fuzz_rta_cell];
/** @brief Stored-tile staging buffer. */
static uint8_t s_scratch[k_fuzz_rta_scratch];
/** @brief Atlas backing copy (the memstore wants a mutable buffer type). */
static uint8_t s_backing[k_fuzz_rta_max_input];

/**
 * @brief libFuzzer entry: parse the fuzz bytes as an atlas, page its tiles.
 *
 * @param[in] data Fuzz input (attacker-controlled atlas bytes).
 * @param[in] size Input length in bytes.
 *
 * @return Always 0 (libFuzzer convention).
 * @retval 0 Input processed (parse/decode succeeded or failed closed).
 *
 * @pre libFuzzer provides a readable buffer of @p size bytes.
 * @pre The static buffers are process-lifetime.
 * @post No heap allocation occurred; any defect surfaces as a sanitizer
 *       diagnostic rather than a return code.
 * @post No state persists between inputs (stateless reader).
 * @note Single-threaded (libFuzzer default).
 * @since 0.1.0
 */
int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
  if ((data == NULL) || (size == 0U) || (size > (size_t)k_fuzz_rta_max_input)) {
    return 0;
  }
  memcpy(s_backing, data, size);
  static ra8_jof_memstore_t store;
  store               = (ra8_jof_memstore_t){.buf = s_backing, .cap = size, .len = size};
  ra8_jof_info_t info = {};
  if (ra8_jof_parse(ra8_jof_memstore_pread, &store, (uint64_t)size, &info) != k_ra8_ok) {
    return 0;
  }
  uint32_t decoded = 0U;
  for (uint16_t ty = 0U; (ty < info.tile_rows) && (decoded < (uint32_t)k_fuzz_rta_max_tiles);
       ty++) {
    for (uint16_t tx = 0U; (tx < info.tile_cols) && (decoded < (uint32_t)k_fuzz_rta_max_tiles);
         tx++) {
      uint16_t w = 0U;
      uint16_t h = 0U;
      (void)ra8_jof_read_tile(ra8_jof_memstore_pread,
                              &store,
                              &info,
                              tx,
                              ty,
                              s_scratch,
                              (uint32_t)sizeof(s_scratch),
                              s_cell,
                              (uint32_t)sizeof(s_cell),
                              &w,
                              &h);
      decoded++;
    }
  }
  return 0;
}
