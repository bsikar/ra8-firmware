/**
 * @file fuzz_ra8_jof_produce.c
 * @brief libFuzzer harness for the #231 import-time transcode producer.
 *
 * @details
 * The producer's source bytes are an image inside a downloaded EPUB -- fully
 * attacker-controlled initial-access content. This harness feeds the raw
 * fuzz input to the exact firmware import path -- `ra8_jof_produce()`
 * over the streaming JPEG stripe decoder and the streaming PNG scanline
 * decoder -- with a fixed work arena and a bounded memstore sink, the same
 * zero-heap strategy the firmware uses. An ASan / UBSan diagnostic here is a
 * real out-of-bounds access or integer overflow in the sniff, marker/chunk
 * parse, inflate, unfilter, band-accumulate or tile-encode stages. The
 * budget caps bound every decode so a valid-but-huge header cannot demand a
 * multi-gigapixel transcode; every random input is expected to fail closed
 * without crashing.
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
#include "ra8_jof_produce.h"

/** @brief Harness sizing: caps chosen so hostile headers stay cheap. */
enum : uint32_t {
  k_fuzz_ta_max_input = 1U << 20,           /**< Cap fuzz input at 1 MiB. */
  k_fuzz_ta_max_dim   = 1024U,              /**< Budget cap per axis.     */
  k_fuzz_ta_tile      = 128U,               /**< Tile edge under fuzz.    */
  k_fuzz_ta_work      = 4U * 1024U * 1024U, /**< Fixed work arena.        */
  k_fuzz_ta_store     = 8U * 1024U * 1024U, /**< Bounded atlas memstore.  */
};

/** @brief Fixed producer work arena (the transcode's whole working set). */
static uint8_t s_work[k_fuzz_ta_work];
/** @brief Bounded atlas store backing. */
static uint8_t s_store_buf[k_fuzz_ta_store];

/**
 * @struct fuzz_pull_t
 * @brief Pull source over the fuzz input.
 */
typedef struct {
  const uint8_t* d;   /**< Fuzz bytes.  */
  size_t         n;   /**< Byte count.  */
  size_t         pos; /**< Read cursor. */
} fuzz_pull_t;

/** @brief ::ra8_jof_pull_fn over the fuzz input. */
static ra8_err_t fuzz_pull(void* ctx, uint8_t* buf, size_t cap, size_t* got)
{
  fuzz_pull_t* p    = (fuzz_pull_t*)ctx;
  const size_t left = p->n - p->pos;
  const size_t take = (cap < left) ? cap : left;
  memcpy(buf, &p->d[p->pos], take);
  p->pos += take;
  *got = take;
  return k_ra8_ok;
}

/**
 * @brief libFuzzer entry: transcode the fuzz bytes as an untrusted image.
 *
 * @param[in] data Fuzz input (attacker-controlled encoded image).
 * @param[in] size Input length in bytes.
 *
 * @return Always 0 (libFuzzer convention).
 * @retval 0 Input processed (transcode succeeded or failed closed).
 *
 * @pre libFuzzer provides a readable buffer of @p size bytes.
 * @pre The static arenas are process-lifetime.
 * @post No heap allocation occurred; any defect surfaces as a sanitizer
 *       diagnostic rather than a return code.
 * @post The memstore holds at most one (possibly partial) atlas.
 * @note Single-threaded (libFuzzer default).
 * @since 0.1.0
 */
int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
  if ((data == NULL) || (size == 0U) || (size > (size_t)k_fuzz_ta_max_input)) {
    return 0;
  }
  static fuzz_pull_t s_pull;
  s_pull = (fuzz_pull_t){.d = data, .n = size, .pos = 0U};
  static ra8_jof_memstore_t s_store;
  s_store = (ra8_jof_memstore_t){.buf = s_store_buf, .cap = sizeof(s_store_buf), .len = 0U};
  /* First input byte picks the codec so both encode paths stay hot. */
  const uint8_t                     codec = (uint8_t)(data[0] & 0x01U);
  const ra8_jof_produce_cfg_t cfg   = {
    .pull       = fuzz_pull,
    .pull_ctx   = &s_pull,
    .sink       = ra8_jof_memstore_sink,
    .sink_ctx   = &s_store,
    .tile_w     = (uint16_t)k_fuzz_ta_tile,
    .tile_h     = (uint16_t)k_fuzz_ta_tile,
    .codec      = codec,
    .max_width  = (uint16_t)k_fuzz_ta_max_dim,
    .max_height = (uint16_t)k_fuzz_ta_max_dim,
    .work       = s_work,
    .work_cap   = sizeof(s_work),
  };
  ra8_jof_info_t info = {};
  (void)ra8_jof_produce(&cfg, &info);
  return 0;
}
