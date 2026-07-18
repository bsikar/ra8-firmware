/**
 * @file ra8_io_stream_ram.c
 * @brief RAM byte-stream sink -- append into a caller-owned buffer.
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Reference sink implementation of ::ra8_io_stream_iface. Writes are a
 * bounds-checked `memcpy` into the caller buffer; a full buffer reports
 * ::k_ra8_err_no_mem. Pure memory access -- identical on host and target.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_io_stream_ram.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_io_stream_internal.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra8_io_stream_ram";

/**
 * @brief RAM sink: append bytes into the caller buffer.
 *
 * @details
 * Copies as many bytes as fit, advances the captured length, and reports the
 * accepted count. Returns ::k_ra8_err_no_mem when not all bytes fit.
 *
 * @param[in]  ctx         RAM sink state (as a void cookie).
 * @param[in]  buf         Source bytes.
 * @param[in]  len         Number of bytes to append.
 * @param[out] out_written Accepted byte count, or NULL.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           All bytes appended.
 * @retval k_ra8_err_null_ptr `ctx` or `buf` was NULL.
 * @retval k_ra8_err_no_mem   The buffer could not accept all bytes.
 *
 * @pre `ctx` is a populated RAM sink state.
 * @pre `buf` is readable for `len` bytes.
 * @post On success the buffer grew by `len` bytes.
 * @post `*out_written` (when provided) holds the accepted count.
 *
 * @note Not thread-safe with respect to the same stream.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t ram_write(void* ctx, const uint8_t* buf, uint32_t len, uint32_t* out_written)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "ctx must not be nullptr");
  RA8_CHECK_NULL_PTR(buf, s_tag, "buf must not be nullptr");
  ra8_io_stream_ram_state_t* st       = (ra8_io_stream_ram_state_t*)ctx;
  const uint32_t             room     = st->cap - st->len;
  const bool                 fits     = (len <= room);
  const uint32_t             accepted = fits ? len : room;
  (void)memcpy(&st->buf[st->len], buf, (size_t)accepted);
  st->len += accepted;
  if (out_written != nullptr) {
    *out_written = accepted;
  }
  return fits ? k_ra8_ok : k_ra8_err_no_mem;
}

/** @brief RAM stream sink vtable. `flush` is NULL: writes land in memory. */
static const ra8_io_stream_iface_t k_ram_iface = {
  .write = ram_write,
  .flush = nullptr,
};

ra8_err_t ra8_io_stream_ram_init(ra8_io_stream_t*           s,
                                 ra8_io_stream_ram_state_t* state,
                                 uint8_t*                   buf,
                                 uint32_t                   cap)
{
  RA8_CHECK_NULL_PTR(s, s_tag, "s must not be nullptr");
  RA8_CHECK_NULL_PTR(state, s_tag, "state must not be nullptr");
  RA8_CHECK_NULL_PTR(buf, s_tag, "buf must not be nullptr");
  if (cap == 0U) {
    return k_ra8_err_invalid_size;
  }
  state->buf = buf;
  state->cap = cap;
  state->len = 0;
  s->iface   = &k_ram_iface;
  s->ctx     = state;
  return k_ra8_ok;
}

ra8_err_t ra8_io_stream_ram_used(const ra8_io_stream_ram_state_t* state, uint32_t* out_used)
{
  RA8_CHECK_NULL_PTR(state, s_tag, "state must not be nullptr");
  RA8_CHECK_NULL_PTR(out_used, s_tag, "out_used must not be nullptr");
  *out_used = state->len;
  return k_ra8_ok;
}
