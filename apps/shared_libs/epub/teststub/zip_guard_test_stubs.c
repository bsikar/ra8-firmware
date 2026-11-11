/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 *
 * Test-only stand-ins for the symbols the zip-guard port links against:
 * miniz (SOUP, stays C) and the decompression-limits policy (ra8_core's
 * Zig module). The Zig tests below assert the guard's own decisions
 * against these stubs; the policy's real behaviour stays ra8_core's to
 * test, and the C++ epub suite drives the real symbols through the
 * archive link. Referenced only by build.zig's test step.
 */
#include <stddef.h>
#include <stdint.h>

typedef struct {
  int dummy;
} mz_zip_archive_stub;

unsigned int teststub_num_files = 0;

typedef struct {
  uint64_t max_output_bytes;
  uint32_t max_ratio;
  uint32_t ratio_grace_bytes;
  uint32_t max_entries;
  uint32_t max_iterations;
  uint8_t  max_depth;
} ra8_decomp_limits_t;

typedef uint16_t ra8_err_t;

unsigned int mz_zip_reader_get_num_files(mz_zip_archive_stub *zip) {
  (void)zip;
  return teststub_num_files;
}

ra8_decomp_limits_t ra8_decomp_limits_default(void) {
  ra8_decomp_limits_t lim;
  lim.max_output_bytes   = 0U;
  lim.max_ratio          = 0U;
  lim.ratio_grace_bytes  = 0U;
  lim.max_entries        = 4U;
  lim.max_iterations     = 0U;
  lim.max_depth          = 0U;
  return lim;
}

ra8_err_t ra8_decomp_check_declared(const ra8_decomp_limits_t *limits, uint64_t comp_size, uint64_t out_size) {
  (void)limits;
  (void)comp_size;
  return (out_size == 0U) ? (ra8_err_t)0 : (ra8_err_t)0x505; /* forwarded-code marker */
}

void ra8_log_emit_error(const char *tag, const char *message) {
  (void)tag;
  (void)message;
}
