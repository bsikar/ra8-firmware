/**
 * @file sweep_block_backends.c
 * @brief The two in-tree #208 sweep backends (`mem`, `rbkc-z9`) + shared
 *        helpers (meter shim, wall clock, deterministic text filler).
 *
 * @details
 * `mem` serves the payload blob straight from resident memory (the harness
 * floor); `rbkc-z9` packs a genuine "RBKC" chunked `.rabook` container in
 * memory with zlib level-9 streams (the same wrapping `tools/epub_compile`
 * emits) and serves it through ::ra8_book_chunked_read, so every cache miss
 * pays a real staged read plus a real tinfl inflate of exactly one chunk.
 * Both implement the ::cbs_backend_t seam declared in sweep_block_internal.h
 * and are published through ::cbs_priv_backends() in report order.
 *
 *
 * [Ring 7 / Tooling] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 *
 *

 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "miniz.h"
#include "ra8_attributes.h"
#include "ra8_book_chunked.h"
#include "ra8_err.h"
#include "sweep_block_internal.h"

/**
 * @enum cbs_seed_t
 * @brief Fixed 64-bit PRNG seed for the deterministic text filler.
 * @details Any odd non-zero value works; this is the splitmix64 golden-ratio
 *          gamma, chosen for good starting bit distribution. Runs are
 *          byte-identical across hosts, so backends see identical payloads.
 * @since 0.1.0
 */
typedef enum : uint64_t {
  k_cbs_seed_fill = 0x9E3779B97F4A7C15ULL, /**< MAGIC-OK: golden-ratio xorshift64 seed */
} cbs_seed_t;

/**
 * @enum cbs_rng_shift_t
 * @brief xorshift64 shift triple for the text filler's PRNG.
 * @details (13, 7, 17) is a full-period parameter set from Marsaglia (2003);
 *          changing any value breaks the period guarantee.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_cbs_shift_a = 13U, /**< MAGIC-OK: xorshift64 left-shift a (Marsaglia 2003 set)  */
  k_cbs_shift_b = 7U,  /**< MAGIC-OK: xorshift64 right-shift b (Marsaglia 2003 set) */
  k_cbs_shift_c = 17U, /**< MAGIC-OK: xorshift64 left-shift c (Marsaglia 2003 set)  */
} cbs_rng_shift_t;

/**
 * @var s_cbs_words
 * @brief Word pool for the deterministic pseudo-text payload.
 * @details Book-prose-like filler so the RBKC backend's zlib streams see a
 *          compression ratio representative of real reflowed chapter text
 *          (roughly 2.5-3x) rather than an artificial ramp that would
 *          understate inflate cost. Read-only after load.
 * @note Only ::cbs_priv_fill_text reads this.
 * @since 0.1.0
 */
static const char* const s_cbs_words[] = {
  "the",     "quick",   "reader", "turns",   "another", "page",    "while",   "morning",
  "light",   "settles", "across", "quiet",   "margins", "and",     "chapter", "headings",
  "gather",  "small",   "notes",  "between", "lines",   "of",      "steady",  "prose",
  "carried", "through", "paper",  "towns",   "by",      "patient", "hands",   "again",
};

/** @brief Implementation of `cbs_priv_now_ns()` -- CLOCK_MONOTONIC fold. */
uint64_t cbs_priv_now_ns(void)
{
  struct timespec ts = {};
  (void)clock_gettime(CLOCK_MONOTONIC, &ts);
  return ((uint64_t)ts.tv_sec * (uint64_t)k_cbs_ns_per_s) + (uint64_t)ts.tv_nsec;
}

/**
 * @brief Fixed-seed xorshift64; deterministic across runs and platforms.
 *
 * @details Applies the Marsaglia (13, 7, 17) xorshift64 triple to @p s in place
 *          and returns the new state word, driving the deterministic text
 *          filler's word selection.
 *
 * @param[in,out] s PRNG state; advanced one step in place.
 *
 * @return uint64_t The updated 64-bit state (the next pseudo-random word).
 * @retval other The post-step state; never 0 given a non-zero seed.
 *
 * @pre @p s is non-NULL and was seeded non-zero.
 * @pre Called on the single benchmark thread.
 * @post `*s` holds the advanced state.
 * @post The sequence is reproducible for a given initial seed.
 *
 * @note Not thread-safe: mutates the caller's state word.
 * @since 0.1.0
 */
static uint64_t cbs_rng(uint64_t* s)
{
  uint64_t x = *s;
  x ^= x << (uint8_t)k_cbs_shift_a;
  x ^= x >> (uint8_t)k_cbs_shift_b;
  x ^= x << (uint8_t)k_cbs_shift_c;
  *s = x;
  return x;
}

/** @brief Implementation of `cbs_priv_fill_text()` -- word-pool xorshift walk. */
void cbs_priv_fill_text(uint8_t* blob, uint32_t len)
{
  if ((blob == nullptr) || (len == 0U)) {
    return;
  }
  const uint32_t nwords = (uint32_t)(sizeof(s_cbs_words) / sizeof(s_cbs_words[0]));
  uint64_t       s      = (uint64_t)k_cbs_seed_fill;
  uint32_t       at     = 0U;
  uint32_t       word   = 0U;
  while (at < len) {
    const char* w = s_cbs_words[(uint32_t)(cbs_rng(&s) % (uint64_t)nwords)];
    for (const char* p = w; (*p != '\0') && (at < len); ++p) {
      blob[at] = (uint8_t)*p;
      at++;
    }
    word++;
    if (word == (uint32_t)k_cbs_words_per_dot) {
      word = 0U;
      if (at < len) {
        blob[at] = (uint8_t)'.';
        at++;
      }
      if (at < len) {
        blob[at] = (uint8_t)'\n';
        at++;
      }
    } else if (at < len) {
      blob[at] = (uint8_t)' ';
      at++;
    }
  }
}

/* ------------------------------------------------------------- metering -- */

/** @brief Implementation of `cbs_priv_meter_read()` -- count, then forward. */
ra8_err_t cbs_priv_meter_read(void* ctx, uint64_t offset, uint8_t* buf, uint32_t len)
{
  cbs_meter_t* m = (cbs_meter_t*)ctx;
  if ((m == nullptr) || (m->inner == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  m->calls++;
  m->bytes += (uint64_t)len;
  return m->inner(m->inner_ctx, offset, buf, len);
}

/* -------------------------------------------------------- `mem` backend -- */

/**
 * @struct cbs_memspan_t
 * @brief A bounded in-memory byte span served through the read seam.
 * @details Backs both the `mem` backend (over the payload blob) and the RBKC
 *          backend's container "file".
 * @invariant `data` covers `len` readable bytes while registered.
 * @since 0.1.0
 */
typedef struct {
  const uint8_t* data; /**< Span base.            */
  uint64_t       len;  /**< Span length in bytes. */
} cbs_memspan_t;

/* cppcheck-suppress constParameterCallback
 * Reason: bound as an ra8_vsource_read_fn, whose signature is
 * `ra8_err_t (*)(void*, uint64_t, uint8_t*, uint32_t)`; constifying ctx
 * would break the function-pointer binding. */
/**
 * @brief `ra8_vsource_read_fn` over a ::cbs_memspan_t (bounds-checked memcpy).
 *
 * @details Copies @p len bytes from `span->data[offset]` into @p buf after
 *          confirming the request lies within the span, so it serves both the
 *          `mem` backend's blob and the RBKC container file through one reader.
 *
 * @param[in]  ctx    The ::cbs_memspan_t to read from (as `void*`).
 * @param[in]  offset Byte offset into the span.
 * @param[out] buf    Destination buffer for @p len bytes.
 * @param[in]  len    Bytes to copy.
 *
 * @return ra8_err_t Read status.
 * @retval k_ra8_ok               @p len bytes were copied into @p buf.
 * @retval k_ra8_err_null_ptr     @p ctx or @p buf is NULL.
 * @retval k_ra8_err_out_of_range `offset + len` exceeds the span length.
 *
 * @pre @p ctx points at a ::cbs_memspan_t whose `data` covers `len` bytes.
 * @pre @p buf covers @p len writable bytes.
 * @post On success, @p buf holds `span->data[offset .. offset+len)`.
 * @post The span and its backing bytes are unmodified.
 *
 * @note Not thread-safe: reads the shared span binding.
 * @since 0.1.0
 */
static ra8_err_t cbs_memspan_read(void* ctx, uint64_t offset, uint8_t* buf, uint32_t len)
{
  const cbs_memspan_t* sp = (const cbs_memspan_t*)ctx;
  if ((sp == nullptr) || (buf == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if ((offset + (uint64_t)len) > sp->len) {
    return k_ra8_err_out_of_range;
  }
  memcpy(buf, &sp->data[offset], (size_t)len);
  return k_ra8_ok;
}

/**
 * @var s_cbs_mem_span
 * @brief The `mem` backend's blob binding for the current setup.
 * @details Rebound on every `setup`; single-threaded tool, so one static
 *          instance suffices.
 * @warning Only ::cbs_mem_setup / ::cbs_mem_teardown may write this.
 * @since 0.1.0
 */
static cbs_memspan_t s_cbs_mem_span = {};

/**
 * @brief `mem` backend setup: serve the blob itself (the harness floor).
 *
 * @details Binds the module-static ::s_cbs_mem_span to @p blob and publishes
 *          ::cbs_memspan_read as the backend reader, so `mem` delivers the
 *          payload straight from resident memory with no per-miss decode -- the
 *          throughput floor the chunked backend is measured against. The block
 *          size is ignored (one resident blob serves every size).
 *
 * @param[in,out] be          Backend to bind (`read`/`read_ctx`/counters set).
 * @param[in]     blob        Source payload to serve.
 * @param[in]     blob_bytes  Payload length in bytes (> 0).
 * @param[in]     block_bytes Swept block size (unused; one blob serves all).
 *
 * @return int 0 on success, 1 on a NULL/zero argument.
 * @retval 0 @p be serves @p blob; `backing_bytes == blob_bytes`.
 * @retval 1 @p be or @p blob was NULL, or @p blob_bytes was 0.
 *
 * @pre @p be is a registered backend entry; @p blob is non-NULL.
 * @pre No prior `mem` setup is live (teardown ran, or first use).
 * @post On 0, `be->read` is ::cbs_memspan_read and `src_bytes` is NULL.
 * @post On 1, no binding is published.
 *
 * @note Not thread-safe: (re)binds the module-static ::s_cbs_mem_span.
 * @since 0.1.0
 */
static int
cbs_mem_setup(cbs_backend_t* be, const uint8_t* blob, uint32_t blob_bytes, uint32_t block_bytes)
{
  (void)block_bytes; /* one resident blob serves every block size */
  if ((be == nullptr) || (blob == nullptr) || (blob_bytes == 0U)) {
    return 1;
  }
  s_cbs_mem_span    = (cbs_memspan_t){.data = blob, .len = (uint64_t)blob_bytes};
  be->read          = cbs_memspan_read;
  be->read_ctx      = &s_cbs_mem_span;
  be->backing_bytes = (uint64_t)blob_bytes;
  be->src_bytes     = nullptr; /* raw medium bytes == delivered bytes */
  return 0;
}

/**
 * @brief `mem` backend teardown: drop the blob binding.
 *
 * @details Clears the module-static ::s_cbs_mem_span and nulls the backend's
 *          `read`/`read_ctx` so a stale blob pointer cannot be dereferenced
 *          after a sweep. Idempotent and NULL-tolerant.
 *
 * @param[in,out] be Backend to unbind (NULL tolerated as a partial no-op).
 *
 * @pre The backend, if bound, was set up by ::cbs_mem_setup.
 * @pre Called on the single benchmark thread.
 * @post ::s_cbs_mem_span is zeroed.
 * @post `be->read` and `be->read_ctx` are NULL when @p be is non-NULL.
 *
 * @note Not thread-safe: clears the module-static binding.
 * @since 0.1.0
 */
static void cbs_mem_teardown(cbs_backend_t* be)
{
  s_cbs_mem_span = (cbs_memspan_t){};
  if (be != nullptr) {
    be->read     = nullptr;
    be->read_ctx = nullptr;
  }
}

/* ---------------------------------------------------- `rbkc-z9` backend -- */

/**
 * @struct cbs_rbkc_t
 * @brief One packed in-memory RBKC container + its bound chunk reader.
 * @details `span` presents the container bytes as the "file"; `file_meter`
 *          wraps that file so raw compressed traffic is counted separately
 *          from the inflated bytes the cache receives.
 * @invariant Buffers are either all live (after a successful setup) or all
 *            freed (after teardown).
 * @since 0.1.0
 */
typedef struct {
  uint8_t*           container;  /**< Packed container bytes (heap).       */
  uint64_t           file_len;   /**< Container length in bytes.           */
  cbs_memspan_t      span;       /**< The container presented as a file.   */
  cbs_meter_t        file_meter; /**< Counts raw container-byte traffic.   */
  uint64_t*          table;      /**< Chunk-table buffer for the reader.   */
  uint8_t*           staging;    /**< Compressed-chunk staging buffer.     */
  ra8_book_chunked_t rd;         /**< The bound demand-paged chunk reader. */
} cbs_rbkc_t;

/**
 * @var s_cbs_rbkc
 * @brief The RBKC backend's state for the current setup.
 * @details Rebuilt per (blob, block size) pair; single-threaded tool, so one
 *          static instance suffices.
 * @warning Only ::cbs_rbkc_setup / ::cbs_rbkc_teardown may (re)bind this.
 * @since 0.1.0
 */
static cbs_rbkc_t s_cbs_rbkc = {};

/**
 * @var s_cbs_tinfl
 * @brief Static tinfl state (~11 KiB) kept off the stack, as the shelf does.
 * @details Reused across inflate calls; re-initialised per stream.
 * @warning Only ::cbs_inflate may touch this.
 * @since 0.1.0
 */
static tinfl_decompressor s_cbs_tinfl;

/**
 * @brief zlib inflater matching `ra8_book_inflate_fn` (mirrors the shelf's).
 *
 * @details Re-initialises the module-static ::s_cbs_tinfl and inflates one zlib
 *          stream from @p src into @p dst in a single non-wrapping pass (the
 *          same tinfl configuration the e-reader shelf uses), so each RBKC cache
 *          miss pays a real one-chunk decompress.
 *
 * @param[in]  src     Compressed zlib stream.
 * @param[in]  src_len Compressed length in bytes.
 * @param[out] dst     Output buffer for the inflated bytes.
 * @param[in]  dst_cap Capacity of @p dst in bytes.
 * @param[out] out_len Receives the inflated byte count on success.
 *
 * @return ra8_err_t Inflate status.
 * @retval k_ra8_ok               The stream inflated; `*out_len` is set.
 * @retval k_ra8_err_null_ptr     @p src, @p dst, or @p out_len is NULL.
 * @retval k_ra8_err_invalid_size The stream did not decode to a complete blob.
 *
 * @pre @p dst_cap is at least the chunk's uncompressed size.
 * @pre @p src holds a complete zlib stream of @p src_len bytes.
 * @post On success, `dst[0..*out_len)` holds the inflated chunk.
 * @post ::s_cbs_tinfl has been reset for this call (no cross-call carryover).
 *
 * @note Not thread-safe: reuses the static tinfl decompressor.
 * @since 0.1.0
 */
static ra8_err_t
cbs_inflate(const void* src, size_t src_len, void* dst, size_t dst_cap, size_t* out_len)
{
  if ((src == nullptr) || (dst == nullptr) || (out_len == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  tinfl_init(&s_cbs_tinfl);
  size_t             in_n  = src_len;
  size_t             out_n = dst_cap;
  const tinfl_status st    = tinfl_decompress(
    &s_cbs_tinfl,
    (const mz_uint8*)src,
    &in_n,
    (mz_uint8*)dst,
    (mz_uint8*)dst,
    &out_n,
    (mz_uint32)(TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF));
  if (st != TINFL_STATUS_DONE) {
    return k_ra8_err_invalid_size;
  }
  *out_len = out_n;
  return k_ra8_ok;
}

/**
 * @brief Write the fixed RBKC header (magic + geometry) into @p out.
 *
 * @details Lays down the "RBKC" magic followed by the block size, total
 *          uncompressed length, chunk count, and a reserved word, matching the
 *          container header ::ra8_book_chunked_open validates.
 *
 * @param[out] out         Container buffer; the header is written at offset 0.
 * @param[in]  block_bytes Chunk size recorded in the header.
 * @param[in]  total       Total uncompressed payload length.
 * @param[in]  count       Number of chunks in the container.
 *
 * @pre @p out has at least `k_ra8_book_container_header_len` writable bytes.
 * @pre Called before the chunk table and payload are written.
 * @post The header region of @p out holds the magic and geometry fields.
 * @post No bytes past the header region are touched.
 *
 * @note Not thread-safe: writes the caller's buffer.
 * @since 0.1.0
 */
static void cbs_rbkc_header(uint8_t* out, uint32_t block_bytes, uint64_t total, uint32_t count)
{
  size_t pos = 0U;
  memcpy(&out[pos], "RBKC", sizeof("RBKC") - 1U);
  pos += sizeof("RBKC") - 1U;
  memcpy(&out[pos], &block_bytes, sizeof(block_bytes));
  pos += sizeof(block_bytes);
  memcpy(&out[pos], &total, sizeof(total));
  pos += sizeof(total);
  memcpy(&out[pos], &count, sizeof(count));
  pos += sizeof(count);
  const uint32_t reserved = 0U;
  memcpy(&out[pos], &reserved, sizeof(reserved));
}

/**
 * @brief Pack an RBKC container over @p blob with real zlib level-9 streams.
 *
 * @details Compresses each `block_bytes` slice with `mz_compress2` at
 *          `MZ_BEST_COMPRESSION` (the level `tools/epub_compile` uses),
 *          writing each stream straight into its final position and each
 *          payload-relative end offset straight into the chunk table, so the
 *          container is assembled in a single pass with no staging copy.
 *
 * @param[in]  blob        Source payload.
 * @param[in]  blob_bytes  Payload length in bytes (> 0).
 * @param[in]  block_bytes Chunk size in bytes (> 0).
 * @param[out] out         Container buffer (capacity @p out_cap).
 * @param[in]  out_cap     Capacity of @p out in bytes.
 * @param[out] out_len     Receives the packed container length.
 *
 * @return int 0 on success, 1 on a compression failure.
 * @retval 0 A valid RBKC container was written; `*out_len` is set.
 * @retval 1 A NULL/zero argument or an `mz_compress2` call failed.
 *
 * @pre @p out_cap covers header + table + `count * mz_compressBound(block)`.
 * @pre @p out and @p out_len are non-NULL.
 * @post On 0, `out[0..*out_len)` is a valid RBKC container over @p blob.
 * @post On 1, @p out contents are unspecified.
 *
 * @note Not thread-safe (uses no shared state, but the caller's buffers).
 * @since 0.1.0
 */
static int cbs_rbkc_pack(const uint8_t* blob,
                         uint32_t       blob_bytes,
                         uint32_t       block_bytes,
                         uint8_t*       out,
                         uint64_t       out_cap,
                         uint64_t*      out_len)
{
  if ((blob == nullptr) || (out == nullptr) || (out_len == nullptr) || (block_bytes == 0U)) {
    return 1;
  }
  const uint32_t count     = (blob_bytes + block_bytes - 1U) / block_bytes;
  const uint64_t table_off = (uint64_t)k_ra8_book_container_header_len;
  const uint64_t payload_off =
    table_off + (((uint64_t)count + 1U) * k_ra8_book_container_entry_len);
  cbs_rbkc_header(out, block_bytes, (uint64_t)blob_bytes, count);
  uint64_t cur = 0U;
  memcpy(&out[table_off], &cur, sizeof(cur));
  for (uint32_t i = 0U; i < count; ++i) {
    const uint32_t at   = i * block_bytes;
    uint32_t       span = blob_bytes - at;
    if (span > block_bytes) {
      span = block_bytes;
    }
    mz_ulong  dst_len = (mz_ulong)(out_cap - (payload_off + cur));
    const int rc      = mz_compress2(&out[payload_off + cur],
                                     &dst_len,
                                     &blob[at],
                                     (mz_ulong)span,
                                     MZ_BEST_COMPRESSION);
    if (rc != MZ_OK) {
      return 1;
    }
    cur += (uint64_t)dst_len;
    memcpy(&out[table_off + (((uint64_t)i + 1U) * k_ra8_book_container_entry_len)],
           &cur,
           sizeof(cur));
  }
  *out_len = payload_off + cur;
  return 0;
}

/**
 * @brief RBKC backend teardown: free the container + reader buffers.
 *
 * @details Frees the packed container, chunk table, and staging buffer held in
 *          the module-static ::s_cbs_rbkc, zeroes it, and nulls the backend's
 *          reader and `src_bytes` pointer. Idempotent and NULL-tolerant, so it
 *          also serves as the failure-path cleanup for ::cbs_rbkc_setup.
 *
 * @param[in,out] be Backend to unbind (NULL tolerated as a partial no-op).
 *
 * @pre The state, if live, was built by ::cbs_rbkc_setup.
 * @pre Called on the single benchmark thread.
 * @post ::s_cbs_rbkc is zeroed and its three buffers are freed.
 * @post `be->read`, `read_ctx`, and `src_bytes` are NULL when @p be is non-NULL.
 *
 * @note Not thread-safe: clears the module-static ::s_cbs_rbkc.
 * @since 0.1.0
 */
static void cbs_rbkc_teardown(cbs_backend_t* be)
{
  free(s_cbs_rbkc.container);
  free(s_cbs_rbkc.table);
  free(s_cbs_rbkc.staging);
  s_cbs_rbkc = (cbs_rbkc_t){};
  if (be != nullptr) {
    be->read      = nullptr;
    be->read_ctx  = nullptr;
    be->src_bytes = nullptr;
  }
}

/**
 * @brief RBKC backend setup: pack a container at @p block_bytes and bind the
 *        demand-paged chunk reader over it.
 *
 * @details Buffer budgets come from `mz_compressBound(block_bytes)` (the
 *          canonical worst-case stream size), so the staging capacity always
 *          covers the largest compressed chunk and the open-path validation
 *          in ::ra8_book_chunked_open cannot reject the container. The
 *          container "file" is read through a nested ::cbs_meter_t so raw
 *          compressed traffic is reported separately.
 *
 * @param[in,out] be          Backend to bind.
 * @param[in]     blob        Source payload.
 * @param[in]     blob_bytes  Payload length in bytes (> 0).
 * @param[in]     block_bytes Chunk size in bytes (> 0).
 *
 * @return int 0 on success, 1 on allocation / pack / open failure.
 * @retval 0 The chunk reader serves the inflated blob via `be->read`.
 * @retval 1 A NULL/zero argument, or an allocation / pack / open step failed.
 *
 * @pre @p be and @p blob are non-NULL.
 * @pre No prior setup is live (teardown ran, or first use).
 * @post On 0, `be->read` serves the inflated flat blob via chunk reads.
 * @post On 1, all partial state is freed (safe to retry or exit).
 *
 * @note Not thread-safe (binds the static ::s_cbs_rbkc).
 * @since 0.1.0
 */
RA8_NASA_RULE_3_OK /* host-only bench: dynamic cache arrays */
  static int
  cbs_rbkc_setup(cbs_backend_t* be, const uint8_t* blob, uint32_t blob_bytes, uint32_t block_bytes)
{
  if ((be == nullptr) || (blob == nullptr) || (blob_bytes == 0U) || (block_bytes == 0U)) {
    return 1;
  }
  cbs_rbkc_t* rb       = &s_cbs_rbkc;
  *rb                  = (cbs_rbkc_t){};
  const uint32_t count = (blob_bytes + block_bytes - 1U) / block_bytes;
  const uint64_t bound = (uint64_t)mz_compressBound((mz_ulong)block_bytes);
  const uint64_t cap   = (uint64_t)k_ra8_book_container_header_len +
                         (((uint64_t)count + 1U) * k_ra8_book_container_entry_len) +
                         ((uint64_t)count * bound);
  rb->container        = (uint8_t*)malloc((size_t)cap);
  rb->table            = (uint64_t*)malloc(((size_t)count + 1U) * sizeof(uint64_t));
  rb->staging          = (uint8_t*)malloc((size_t)bound);
  if ((rb->container == nullptr) || (rb->table == nullptr) || (rb->staging == nullptr)) {
    cbs_rbkc_teardown(be);
    return 1;
  }
  if (cbs_rbkc_pack(blob, blob_bytes, block_bytes, rb->container, cap, &rb->file_len) != 0) {
    cbs_rbkc_teardown(be);
    return 1;
  }
  rb->span            = (cbs_memspan_t){.data = rb->container, .len = rb->file_len};
  rb->file_meter      = (cbs_meter_t){.inner = cbs_memspan_read, .inner_ctx = &rb->span};
  const ra8_err_t err = ra8_book_chunked_open(&rb->rd,
                                              cbs_priv_meter_read,
                                              &rb->file_meter,
                                              rb->file_len,
                                              cbs_inflate,
                                              rb->table,
                                              count + 1U,
                                              rb->staging,
                                              (uint32_t)bound);
  if (err != k_ra8_ok) {
    (void)fprintf(stderr, "sweep-block: rbkc open failed (err=%d)\n", (int)err);
    cbs_rbkc_teardown(be);
    return 1;
  }
  be->read          = ra8_book_chunked_read;
  be->read_ctx      = &rb->rd;
  be->backing_bytes = rb->file_len;
  be->src_bytes     = &rb->file_meter.bytes;
  return 0;
}

/* -------------------------------------------------------------- registry -- */

/**
 * @var s_cbs_backends
 * @brief The registered sweep backends (the seam the HW leg extends).
 * @details Order fixes report order; `mem` first as the floor reference.
 * @warning Setup binds `read`/`read_ctx` in place; single-threaded use only.
 * @since 0.1.0
 */
static cbs_backend_t s_cbs_backends[] = {
  {.name = "mem", .setup = cbs_mem_setup, .teardown = cbs_mem_teardown},
  {.name = "rbkc-z9", .setup = cbs_rbkc_setup, .teardown = cbs_rbkc_teardown},
};

/** @brief Implementation of `cbs_priv_backends()` -- static registry handout. */
cbs_backend_t* cbs_priv_backends(uint32_t* out_count)
{
  if (out_count != nullptr) {
    *out_count = (uint32_t)(sizeof(s_cbs_backends) / sizeof(s_cbs_backends[0]));
  }
  return s_cbs_backends;
}
