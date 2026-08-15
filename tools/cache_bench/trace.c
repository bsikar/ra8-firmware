/**
 * @file trace.c
 * @brief Deterministic synthetic cursors and bounded captured-trace parsing.
 * @details Implements resettable generators plus injected, fingerprinted
 *          captured reads without materializing any complete access trace.
 *
 * [Ring 7 / Tooling] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
#include "trace.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "ra8_attributes.h"

typedef enum : uint32_t {
  k_cb_obj_book       = 1U,       /**< Synthetic book object key.         */
  k_cb_obj_comic      = 2U,       /**< Synthetic comic object key.        */
  k_cb_footprint      = 8192U,    /**< Ordinary working-set pages.        */
  k_cb_accesses       = 120000U,  /**< Ordinary workload access count.    */
  k_cb_hot_pages      = 96U,      /**< Ordinary reread hot-set pages.     */
  k_cb_reread_pct     = 82U,      /**< Reread hot-set percentage.         */
  k_cb_jump_pct       = 4U,       /**< Random-jump percentage.            */
  k_cb_tile_span      = 6144U,    /**< Scroll workload page span.         */
  k_cb_sr_hot         = 192U,     /**< Scan-resistant hot-set pages.      */
  k_cb_sr_hot_pass    = 3U,       /**< Hot passes per scan-resistant run. */
  k_cb_sr_scan        = 1500U,    /**< Scan pages per resistant run.      */
  k_cb_pct_full       = 100U,     /**< Complete percentage denominator.   */
  k_cb_mixed_phase    = 2048U,    /**< Accesses per mixed phase.          */
  k_cb_huge_footprint = 1835008U, /**< Huge workload working-set pages.   */
  k_cb_huge_hot       = 256U,     /**< Huge workload hot-set pages.       */
  k_cb_huge_hot_pass  = 3U,       /**< Huge workload hot passes.          */
  k_cb_huge_scan      = 4000U,    /**< Huge workload scan pages.          */
} cb_workload_dim_t;

typedef enum : uint8_t {
  k_rng_shift_a       = 13U, /**< First xorshift distance.       */
  k_rng_shift_b       = 7U,  /**< Middle xorshift distance.      */
  k_rng_shift_c       = 17U, /**< Final xorshift distance.       */
  k_cb_base_dec       = 10U, /**< Captured decimal parser radix. */
  k_cb_key_high_shift = 24U, /**< Object-key high-word shift.    */
} cb_trace_math_t;

typedef enum : uint64_t {
  k_rng_seed_random  = 0x9E3779B97F4A7C15ULL, /**< Random workload seed.  */
  k_rng_seed_reread  = 0xD1B54A32D192ED03ULL, /**< Reread workload seed.  */
  k_rng_seed_toc     = 0x2545F4914F6CDD1DULL, /**< Jump workload seed.    */
  k_rng_seed_mixed_a = 0x9E3779B97F4A7C15ULL, /**< Mixed hot-phase seed.  */
  k_rng_seed_mixed_b = 0xABCDEF1234567890ULL, /**< Mixed scan-phase seed. */
  k_cb_hash_offset   = 0xCBF29CE484222325ULL, /**< FNV-1a offset basis.   */
  k_cb_hash_prime    = 0x100000001B3ULL,      /**< FNV-1a multiplication. */
} cb_trace_seed_t;

/**
 * @brief Advance a deterministic trace pseudo-random generator.
 * @details Applies the fixed xorshift sequence used by synthetic workloads.
 * @param[in,out] state Non-zero generator state.
 * @return Updated 64-bit pseudo-random value.
 * @retval other Deterministic next value in the sequence.
 * @pre @p state is non-NULL and writable.
 * @pre The initial state is non-zero.
 * @post `*state` equals the returned value.
 * @post No state outside @p state is modified.
 * @note This generator provides reproducibility, not cryptographic entropy.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint64_t internal_rng(uint64_t* state)
{
  uint64_t value = *state;
  value ^= value << (uint8_t)k_rng_shift_a;
  value ^= value >> (uint8_t)k_rng_shift_b;
  value ^= value << (uint8_t)k_rng_shift_c;
  *state = value;
  return value;
}

/**
 * @brief Select a deterministic pseudo-random value below a bound.
 * @details Advances @p state and reduces the result modulo @p span, with a
 *          defined zero result for an empty span.
 * @param[in,out] state Generator state.
 * @param[in] span Exclusive upper bound.
 * @return Value in `[0, span)`, or zero when @p span is zero.
 * @retval 0 The span is zero or the reduced value is zero.
 * @retval other Reduced pseudo-random value below @p span.
 * @pre @p state is non-NULL and initialized.
 * @pre @p span is an ordinary synthetic-workload bound.
 * @post For non-zero @p span, the result is strictly less than @p span.
 * @post @p state advances exactly once.
 * @note Modulo bias is acceptable for this deterministic benchmark corpus.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t internal_rand_below(uint64_t* state, uint32_t span)
{
  return (span == 0U) ? 0U : (uint32_t)(internal_rng(state) % (uint64_t)span);
}

/**
 * @brief Fold one cache key into the captured-trace fingerprint.
 * @details Serializes both 32-bit fields in fixed little-endian byte order and
 *          applies the benchmark's FNV-1a accumulator.
 * @param[in] hash Current fingerprint accumulator.
 * @param[in] key Cache key to append.
 * @return Updated 64-bit fingerprint.
 * @retval other Fingerprint after all eight key bytes are folded.
 * @pre @p hash is the offset basis or a prior result from this helper.
 * @pre @p key contains initialized object and page fields.
 * @post The result is independent of host structure padding and endianness.
 * @post No storage is modified.
 * @note This fingerprint detects mutation; it is not a security hash.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint64_t internal_trace_hash(uint64_t hash, cb_key_t key)
{
  const uint8_t bytes[sizeof(key)] = {
    (uint8_t)key.object_id,
    (uint8_t)(key.object_id >> 8U),
    (uint8_t)(key.object_id >> 16U),
    (uint8_t)(key.object_id >> (uint8_t)k_cb_key_high_shift),
    (uint8_t)key.page,
    (uint8_t)(key.page >> 8U),
    (uint8_t)(key.page >> 16U),
    (uint8_t)(key.page >> (uint8_t)k_cb_key_high_shift),
  };
  for (size_t i = 0U; i < sizeof(bytes); ++i) {
    hash ^= bytes[i];
    hash *= (uint64_t)k_cb_hash_prime;
  }
  return hash;
}

void cb_traces_synthetic(cb_trace_t out[k_cb_synthetic_trace_count])
{
  static const char* const names[k_cb_synthetic_trace_count] = {
    "seq-pageturn",
    "random",
    "reread-locality",
    "linear+jumps",
    "cbz-scroll",
    "hotset+scan",
    "hugebook-7GiB",
    "mixed-session",
  };
  static const uint32_t footprints[k_cb_synthetic_trace_count] = {
    k_cb_footprint,
    k_cb_footprint,
    k_cb_footprint,
    k_cb_footprint,
    k_cb_tile_span,
    k_cb_footprint,
    k_cb_huge_footprint,
    k_cb_footprint,
  };
  for (uint8_t i = 0U; i < (uint8_t)k_cb_synthetic_trace_count; ++i) {
    out[i] = (cb_trace_t){.name      = names[i],
                          .n         = (uint64_t)k_cb_accesses,
                          .footprint = footprints[i],
                          .kind      = (cb_trace_kind_t)i};
  }
}

/**
 * @brief Pull one byte through the captured-source read-ahead buffer.
 * @details Refills the bounded buffer through the injected source at EOF of the
 *          current grain and rejects zero-progress or over-count callbacks.
 * @param[in,out] cursor Open captured trace cursor.
 * @param[out] out Receives one byte when @p eof is false.
 * @param[out] eof Receives true at clean source exhaustion.
 * @return Tool-local I/O status.
 * @retval k_cb_io_ok One byte or clean EOF was reported.
 * @retval k_cb_io_fault The source violated its progress/count contract.
 * @retval k_cb_io_mutated The injected source reported mutation.
 * @pre All pointers are non-NULL and @p cursor is open on a captured trace.
 * @pre The cursor read-ahead indices are within their fixed buffer.
 * @post On a byte, the read cursor advances exactly once.
 * @post At EOF, @p out is not modified and @p eof is true.
 * @note Short injected reads are accepted and buffered.
 * @since 0.1.0
 */
RA8_INTERNAL
static cb_io_status_t internal_trace_read_byte(cb_trace_cursor_t* cursor, uint8_t* out, bool* eof)
{
  if (cursor->read_at == cursor->read_count) {
    if (cursor->source_offset >= cursor->trace->source.size) {
      *eof = true;
      return k_cb_io_ok;
    }
    size_t         count     = 0U;
    const uint64_t remaining = cursor->trace->source.size - cursor->source_offset;
    const size_t   request =
      (remaining < sizeof(cursor->read_buffer)) ? (size_t)remaining : sizeof(cursor->read_buffer);
    const cb_io_status_t status = cursor->trace->source.read(cursor->trace->source.ctx,
                                                             cursor->source_offset,
                                                             cursor->read_buffer,
                                                             request,
                                                             &count);
    if ((status != k_cb_io_ok) || (count == 0U) || (count > request)) {
      return (status == k_cb_io_ok) ? k_cb_io_fault : status;
    }
    cursor->source_offset += count;
    cursor->read_at    = 0U;
    cursor->read_count = count;
  }
  *out = cursor->read_buffer[cursor->read_at];
  cursor->read_at++;
  *eof = false;
  return k_cb_io_ok;
}

/**
 * @brief Parse one captured decimal object/page record.
 * @details Converts the two unsigned decimal fields and rejects missing,
 *          overflowing, or conversion-error values.
 * @param[in] line NUL-terminated record bytes.
 * @param[out] object_id Receives the object identifier.
 * @param[out] page Receives the page index.
 * @return Whether both fields were converted.
 * @retval true Both outputs contain 32-bit values.
 * @retval false A field is absent, overflowing, or invalid.
 * @pre All pointers are non-NULL and outputs are writable.
 * @pre @p line is NUL-terminated within the fixed line capacity.
 * @post On success, both outputs are initialized.
 * @post Input bytes are not modified.
 * @note Trailing bytes preserve the historical permissive parser behavior.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_parse_trace_line(const char* line, uint32_t* object_id, uint32_t* page)
{
  char* end                  = nullptr;
  errno                      = 0;
  const unsigned long object = strtoul(line, &end, (int)k_cb_base_dec);
  if ((end == line) || (errno != 0) || (object > UINT32_MAX)) {
    return false;
  }
  const char* second             = end;
  errno                          = 0;
  const unsigned long page_value = strtoul(second, &end, (int)k_cb_base_dec);
  if ((end == second) || (errno != 0) || (page_value > UINT32_MAX)) {
    return false;
  }
  *object_id = (uint32_t)object;
  *page      = (uint32_t)page_value;
  return true;
}

/**
 * @brief Emit the next key from a captured decimal trace.
 * @details Assembles one bounded line through read-ahead, treats clean EOF as
 *          done, and preserves the historical stop-on-unparseable-line rule.
 * @param[in,out] cursor Open captured trace cursor.
 * @param[out] key Receives the next parsed key.
 * @param[out] done Receives true at EOF or the first unparsable record.
 * @return Tool-local I/O status.
 * @retval k_cb_io_ok A key or clean termination was produced.
 * @retval k_cb_io_fault The injected source contract failed.
 * @retval k_cb_io_mutated The source reported mutation.
 * @pre All pointers are non-NULL and @p cursor is captured-kind.
 * @pre Cursor line and read-ahead buffers are initialized.
 * @post When @p done is false, @p key contains one parsed record.
 * @post Cursor source position never moves backward.
 * @note A line reaching the fixed bound is parsed from its bounded prefix.
 * @since 0.1.0
 */
RA8_INTERNAL
static cb_io_status_t
internal_trace_captured_next(cb_trace_cursor_t* cursor, cb_key_t* key, bool* done)
{
  size_t length = 0U;
  bool   eof    = false;
  while ((length + 1U) < sizeof(cursor->line)) {
    uint8_t              value  = 0U;
    const cb_io_status_t status = internal_trace_read_byte(cursor, &value, &eof);
    if (status != k_cb_io_ok) {
      return status;
    }
    if (eof) {
      break;
    }
    cursor->line[length] = (char)value;
    length++;
    if (value == (uint8_t)'\n') {
      break;
    }
  }
  if ((length == 0U) && eof) {
    *done = true;
    return k_cb_io_ok;
  }
  cursor->line[length] = '\0';
  if (!internal_parse_trace_line(cursor->line, &key->object_id, &key->page)) {
    *done = true;
    return k_cb_io_ok;
  }
  *done = false;
  return k_cb_io_ok;
}

/**
 * @brief Generate one scan-resistant hot-set-plus-scan workload key.
 * @details Alternates repeated hot-set passes with a moving cold scan and
 *          supports both normal and huge-book geometries.
 * @param[in] index Zero-based access index.
 * @param[in] huge Selects the huge-book geometry when true.
 * @return Deterministic cache key for @p index.
 * @retval other Book-object key within the selected footprint.
 * @pre The selected footprint is larger than its hot set.
 * @pre Fixed hot, pass, and scan constants define a non-zero cycle.
 * @post The returned page is less than the selected footprint.
 * @post No generator state is retained or modified.
 * @note Thread-safe: this is a pure function of @p index and @p huge.
 * @since 0.1.0
 */
RA8_INTERNAL
static cb_key_t internal_trace_scan_key(uint64_t index, bool huge)
{
  const uint32_t hot       = huge ? (uint32_t)k_cb_huge_hot : (uint32_t)k_cb_sr_hot;
  const uint32_t passes    = huge ? (uint32_t)k_cb_huge_hot_pass : (uint32_t)k_cb_sr_hot_pass;
  const uint32_t scan      = huge ? (uint32_t)k_cb_huge_scan : (uint32_t)k_cb_sr_scan;
  const uint32_t footprint = huge ? (uint32_t)k_cb_huge_footprint : (uint32_t)k_cb_footprint;
  const uint64_t hot_span  = (uint64_t)hot * (uint64_t)passes;
  const uint64_t cycle     = hot_span + (uint64_t)scan;
  const uint64_t round     = index / cycle;
  const uint64_t pos       = index % cycle;
  const uint32_t page = (pos < hot_span)
                          ? (uint32_t)(pos % hot)
                          : hot + (uint32_t)(((uint64_t)hot + (round * scan) + (pos - hot_span)) %
                                             (uint64_t)(footprint - hot));
  return (cb_key_t){.object_id = k_cb_obj_book, .page = page};
}

/**
 * @brief Emit one key from the selected synthetic workload generator.
 * @details Dispatches by trace kind and advances only the cursor state required
 *          by that workload's locality pattern.
 * @param[in,out] cursor Open synthetic trace cursor.
 * @return Next deterministic cache key.
 * @retval other Key within the selected workload geometry.
 * @pre @p cursor and `cursor->trace` are non-NULL.
 * @pre `cursor->trace->kind` is a synthetic kind.
 * @post Returned object and page fields are initialized.
 * @post Any stateful generator fields advance consistently with one access.
 * @note The public cursor function advances the common access index.
 * @since 0.1.0
 */
RA8_INTERNAL
static cb_key_t internal_trace_synthetic_next(cb_trace_cursor_t* cursor)
{
  const uint64_t index = cursor->index;
  switch (cursor->trace->kind) {
    case k_cb_trace_seq:
      return (cb_key_t){.object_id = k_cb_obj_book,
                        .page      = (uint32_t)(index % (uint64_t)k_cb_footprint)};
    case k_cb_trace_random:
      return (cb_key_t){.object_id = k_cb_obj_book,
                        .page      = internal_rand_below(&cursor->rng, k_cb_footprint)};
    case k_cb_trace_reread: {
      uint32_t page = 0U;
      if (internal_rand_below(&cursor->rng, k_cb_pct_full) < k_cb_reread_pct) {
        page = cursor->hot + internal_rand_below(&cursor->rng, k_cb_hot_pages);
      } else {
        page        = internal_rand_below(&cursor->rng, k_cb_footprint);
        cursor->hot = (page < k_cb_hot_pages) ? 0U : (page - k_cb_hot_pages);
      }
      return (cb_key_t){.object_id = k_cb_obj_book, .page = page % k_cb_footprint};
    }
    case k_cb_trace_jumps:
      cursor->page = (internal_rand_below(&cursor->rng, k_cb_pct_full) < k_cb_jump_pct)
                       ? internal_rand_below(&cursor->rng, k_cb_footprint)
                       : (cursor->page + 1U) % k_cb_footprint;
      return (cb_key_t){.object_id = k_cb_obj_book, .page = cursor->page};
    case k_cb_trace_scroll:
      return (cb_key_t){.object_id = k_cb_obj_comic,
                        .page      = (uint32_t)(index % (uint64_t)k_cb_tile_span)};
    case k_cb_trace_scan:
      return internal_trace_scan_key(index, false);
    case k_cb_trace_huge:
      return internal_trace_scan_key(index, true);
    case k_cb_trace_mixed: {
      const uint32_t phase = (uint32_t)((index / k_cb_mixed_phase) % 4U);
      if (phase == 0U) {
        cursor->page = (cursor->page + 1U) % k_cb_footprint;
      } else if (phase == 1U) {
        cursor->page = cursor->hot + internal_rand_below(&cursor->rng, k_cb_hot_pages);
      } else if (phase == 2U) {
        cursor->page = internal_rand_below(&cursor->rng, k_cb_footprint);
        cursor->hot  = (cursor->page < k_cb_hot_pages) ? 0U : cursor->page - k_cb_hot_pages;
      } else {
        return (cb_key_t){.object_id = k_cb_obj_comic, .page = cursor->page % k_cb_tile_span};
      }
      return (cb_key_t){.object_id = k_cb_obj_book, .page = cursor->page % k_cb_footprint};
    }
    case k_cb_trace_captured:
      break;
  }
  return (cb_key_t){};
}

cb_io_status_t cb_trace_cursor_open(const cb_trace_t* trace, cb_trace_cursor_t* cursor)
{
  if ((trace == nullptr) || (cursor == nullptr)) {
    return k_cb_io_fault;
  }
  *cursor = (cb_trace_cursor_t){.trace = trace, .fingerprint = (uint64_t)k_cb_hash_offset};
  if (trace->kind == k_cb_trace_random) {
    cursor->rng = (uint64_t)k_rng_seed_random;
  } else if (trace->kind == k_cb_trace_reread) {
    cursor->rng = (uint64_t)k_rng_seed_reread;
  } else if (trace->kind == k_cb_trace_jumps) {
    cursor->rng = (uint64_t)k_rng_seed_toc;
  } else if (trace->kind == k_cb_trace_mixed) {
    cursor->rng = (uint64_t)k_rng_seed_mixed_a ^ (uint64_t)k_rng_seed_mixed_b;
  } else if ((trace->kind == k_cb_trace_captured) && (trace->source.read == nullptr)) {
    return k_cb_io_fault;
  }
  return k_cb_io_ok;
}

cb_io_status_t cb_trace_cursor_next(cb_trace_cursor_t* cursor, cb_key_t* key, bool* done)
{
  if ((cursor == nullptr) || (cursor->trace == nullptr) || (key == nullptr) || (done == nullptr)) {
    return k_cb_io_fault;
  }
  cb_io_status_t status = k_cb_io_ok;
  if (cursor->trace->kind == k_cb_trace_captured) {
    status = internal_trace_captured_next(cursor, key, done);
  } else {
    *done = cursor->index >= cursor->trace->n;
    if (!*done) {
      *key = internal_trace_synthetic_next(cursor);
    }
  }
  if ((status == k_cb_io_ok) && !*done) {
    cursor->fingerprint = internal_trace_hash(cursor->fingerprint, *key);
    cursor->index++;
  }
  return status;
}

cb_io_status_t cb_trace_cursor_finish(const cb_trace_cursor_t* cursor)
{
  if ((cursor == nullptr) || (cursor->trace == nullptr)) {
    return k_cb_io_fault;
  }
  if ((cursor->index != cursor->trace->n) ||
      ((cursor->trace->kind == k_cb_trace_captured) &&
       (cursor->fingerprint != cursor->trace->fingerprint))) {
    return k_cb_io_mutated;
  }
  return k_cb_io_ok;
}

cb_io_status_t
cb_trace_bind(const cb_source_t* source, const char* name, size_t name_length, cb_trace_t* out)
{
  if ((source == nullptr) || (source->read == nullptr) || (name == nullptr) || (out == nullptr) ||
      (name_length == 0U) || (name_length >= (size_t)k_cb_trace_name_capacity)) {
    return k_cb_io_capacity;
  }
  *out = (cb_trace_t){.kind = k_cb_trace_captured, .source = *source};
  memcpy(out->name_storage, name, name_length);
  out->name_storage[name_length] = '\0';
  out->name                      = out->name_storage;
  cb_trace_cursor_t cursor       = {};
  cb_io_status_t    status       = cb_trace_cursor_open(out, &cursor);
  bool              done         = false;
  while ((status == k_cb_io_ok) && !done) {
    cb_key_t key = {};
    status       = cb_trace_cursor_next(&cursor, &key, &done);
  }
  if ((status != k_cb_io_ok) || (cursor.index == 0U)) {
    *out = (cb_trace_t){};
    return (status == k_cb_io_ok) ? k_cb_io_fault : status;
  }
  out->n           = cursor.index;
  out->fingerprint = cursor.fingerprint;
  return k_cb_io_ok;
}
