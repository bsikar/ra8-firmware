/**
 * @file sweep_block.c
 * @brief Implementation of the #208 block/frame-size sweep (`--sweep-block`).
 *
 * @details
 * Drives the REAL ::ra8_vmem SLRU page cache (not a re-modelled policy) with
 * `frame_bytes` swept from 512 B to 256 KiB under a constant byte budget, over
 * two backends behind the ::cbs_backend_t seam: a plain resident-memory blob
 * (the harness floor) and a genuine "RBKC" chunked `.rabook` container packed
 * in memory with zlib level-9 streams (the same wrapping `tools/epub_compile`
 * emits) and served through ::ra8_book_chunked_read, so every cache miss pays a
 * real staged read plus a real tinfl inflate of exactly one chunk. Leg (a)
 * scans the whole object sequentially; leg (b) re-reads one block. Every byte
 * handed back by the cache is verified against the source blob.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 7 / Tooling] {World: NS}
 *
 * @since 0.1.0
 */
#include "sweep_block.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "miniz.h"
#include "ra8_book_chunked.h"
#include "ra8_err.h"
#include "ra8_vmem.h"
#include "ra8_vsource.h"

/**
 * @enum cbs_block_size_t
 * @brief The swept block / frame / chunk sizes, in bytes.
 * @details Powers of two from one SD sector up to a quarter MiB, bracketing
 *          the 64 KiB `.rabook` chunk default (#204) by two octaves on each
 *          side so the knee is visible whichever way it falls.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_cbs_block_512b   = 512U,    /**< One SD sector.                       */
  k_cbs_block_1kib   = 1024U,   /**< 1 KiB.                               */
  k_cbs_block_4kib   = 4096U,   /**< Classic VM page size.                */
  k_cbs_block_16kib  = 16384U,  /**< 16 KiB.                              */
  k_cbs_block_64kib  = 65536U,  /**< The current `.rabook` chunk default. */
  k_cbs_block_256kib = 262144U, /**< 256 KiB (frame RAM gets expensive).  */
} cbs_block_size_t;

/**
 * @enum cbs_dim_t
 * @brief Workload geometry and fixed sweep parameters.
 * @details The payload is a multiple of every swept block size and of the
 *          reader request grain, so no leg ever sees a partial-block edge;
 *          the cache byte budget is held constant across sizes (same RAM
 *          spend, different frame counts) for an honest comparison.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_cbs_blob_bytes    = 8388608U,    /**< 8 MiB payload (2^23; all sizes divide it). */
  k_cbs_cache_bytes   = 1048576U,    /**< 1 MiB resident cache budget (constant).    */
  k_cbs_req_bytes     = 256U,        /**< Reader request grain (divides every size). */
  k_cbs_seq_passes    = 4U,          /**< Whole-object passes in the seq leg.        */
  k_cbs_hot_reads     = 1048576U,    /**< Accesses in the same-block re-read leg.    */
  k_cbs_bucket_min    = 16U,         /**< Minimum hash-bucket count for tiny caches. */
  k_cbs_default_chunk = 65536U,      /**< #204 `.rabook` chunk-size default.         */
  k_cbs_knee_pct      = 90U,         /**< %% of peak throughput that names the knee. */
  k_cbs_words_per_dot = 11U,         /**< Words per sentence in the text filler.     */
  k_cbs_kib           = 1024U,       /**< Bytes per KiB (block-size labels).         */
  k_cbs_ns_per_us     = 1000U,       /**< Nanoseconds per microsecond.               */
  k_cbs_ns_per_s      = 1000000000U, /**< Nanoseconds per second.                    */
  k_cbs_max_rows      = 24U,         /**< 2 backends x 6 sizes x 2 legs.             */
} cbs_dim_t;

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

/** @brief Log backend stub so ra8_check's RA8_CHECK_* macros link host-side. */
void internal_ra8_log_error(const char* tag, const char* message)
{
  (void)fprintf(stderr, "[ra8_log] %s: %s\n", tag, message);
}

/** @brief Valued log backend stub (present for the linker, as reader_vmem). */
void internal_ra8_log_error_val(const char* tag, const char* message, uint32_t value)
{
  (void)fprintf(stderr, "[ra8_log] %s: %s =%u\n", tag, message, value);
}

/** @brief 100.0 as a double, for percentage maths. */
static const double s_cbs_pct_f = 100.0;
/** @brief Bytes per MiB as a double, for throughput maths. */
static const double s_cbs_mib_f = 1048576.0;
/** @brief Nanoseconds per second as a double, for throughput maths. */
static const double s_cbs_ns_per_s_f = 1000000000.0;
/** @brief Nanoseconds per microsecond as a double, for latency maths. */
static const double s_cbs_ns_per_us_f = 1000.0;

/**
 * @var s_cbs_words
 * @brief Word pool for the deterministic pseudo-text payload.
 * @details Book-prose-like filler so the RBKC backend's zlib streams see a
 *          compression ratio representative of real reflowed chapter text
 *          (roughly 2.5-3x) rather than an artificial ramp that would
 *          understate inflate cost. Read-only after load.
 * @note Only ::cbs_fill_text reads this.
 * @since 0.1.0
 */
static const char* const s_cbs_words[] = {
  "the",     "quick",   "reader", "turns",   "another", "page",    "while",   "morning",
  "light",   "settles", "across", "quiet",   "margins", "and",     "chapter", "headings",
  "gather",  "small",   "notes",  "between", "lines",   "of",      "steady",  "prose",
  "carried", "through", "paper",  "towns",   "by",      "patient", "hands",   "again",
};

/** @brief Monotonic wall-clock in nanoseconds. */
static uint64_t cbs_now_ns(void)
{
  struct timespec ts = {};
  (void)clock_gettime(CLOCK_MONOTONIC, &ts);
  return ((uint64_t)ts.tv_sec * (uint64_t)k_cbs_ns_per_s) + (uint64_t)ts.tv_nsec;
}

/** @brief Fixed-seed xorshift64; deterministic across runs and platforms. */
static uint64_t cbs_rng(uint64_t* s)
{
  uint64_t x = *s;
  x ^= x << (uint8_t)k_cbs_shift_a;
  x ^= x >> (uint8_t)k_cbs_shift_b;
  x ^= x << (uint8_t)k_cbs_shift_c;
  *s = x;
  return x;
}

/** @brief Round @p v up to a power of two (>= 1). */
static uint32_t cbs_pow2_ceil(uint32_t v)
{
  uint32_t p = 1U;
  while (p < v) {
    p <<= 1U;
  }
  return p;
}

/** @brief Fill @p blob with deterministic book-like pseudo-text. */
static void cbs_fill_text(uint8_t* blob, uint32_t len)
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

/**
 * @struct cbs_meter_t
 * @brief Counting shim around an ::ra8_vsource_read_fn.
 * @details Forwards to `inner` and tallies calls + bytes. One instance sits
 *          at the vsource seam (counting storage commands = cache misses);
 *          the RBKC backend nests a second one under its container file to
 *          count raw compressed bytes.
 * @invariant `inner` is non-NULL whenever the meter is registered as a reader.
 * @since 0.1.0
 */
typedef struct {
  ra8_vsource_read_fn inner;     /**< Wrapped reader.                */
  void*               inner_ctx; /**< Context for @ref inner.        */
  uint64_t            calls;     /**< Read calls forwarded so far.   */
  uint64_t            bytes;     /**< Bytes served through the shim. */
} cbs_meter_t;

/** @brief `ra8_vsource_read_fn` forwarding through a ::cbs_meter_t. */
static ra8_err_t cbs_meter_read(void* ctx, uint64_t offset, uint8_t* buf, uint32_t len)
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

/** @brief `ra8_vsource_read_fn` over a ::cbs_memspan_t (bounds-checked memcpy). */
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

/** @brief `mem` backend setup: serve the blob itself (the harness floor). */
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

/** @brief `mem` backend teardown: drop the blob binding. */
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

/** @brief zlib inflater matching `ra8_book_inflate_fn` (mirrors the shelf's). */
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

/** @brief Write the fixed RBKC header (magic + geometry) into @p out. */
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

/** @brief RBKC backend teardown: free the container + reader buffers. */
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
 *
 * @pre @p be and @p blob are non-NULL.
 * @pre No prior setup is live (teardown ran, or first use).
 * @post On 0, `be->read` serves the inflated flat blob via chunk reads.
 * @post On 1, all partial state is freed (safe to retry or exit).
 *
 * @note Not thread-safe (binds the static ::s_cbs_rbkc).
 * @since 0.1.0
 */
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
                                              cbs_meter_read,
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

/* ----------------------------------------------------------- the sweep -- */

/**
 * @struct cbs_cache_t
 * @brief One ::ra8_vmem instance sized for a swept block size + its storage.
 * @details Frame storage, metadata, and buckets are heap-carved per leg so
 *          every leg starts cold; the vsource-level ::cbs_meter_t counts
 *          storage commands (== misses) and delivered bytes.
 * @invariant `frames == k_cbs_cache_bytes / frame_bytes` (>= 1).
 * @since 0.1.0
 */
typedef struct {
  ra8_vmem_t        vm;        /**< The real SLRU page cache under test. */
  ra8_vsource_t     vs;        /**< Object-source registry (one object). */
  ra8_vsource_obj_t objs[1];   /**< The single registered object's slot. */
  cbs_meter_t       meter;     /**< Vsource-seam storage-command meter.  */
  uint8_t*          frame_mem; /**< `frames * block` page storage.       */
  ra8_vmem_frame_t* meta;      /**< Per-frame metadata array.            */
  int32_t*          buckets;   /**< Hash-bucket heads.                   */
  uint32_t          frames;    /**< Frame count at this block size.      */
  uint32_t          object_id; /**< Registered object id.                */
} cbs_cache_t;

/** @brief Release a ::cbs_cache_t's heap carvings (idempotent). */
static void cbs_cache_close(cbs_cache_t* c)
{
  if (c == nullptr) {
    return;
  }
  free(c->frame_mem);
  free(c->meta);
  free(c->buckets);
  *c = (cbs_cache_t){};
}

/**
 * @brief Stand up a real ::ra8_vmem cache with `frame_bytes == block_bytes`.
 *
 * @details The byte budget ::k_cbs_cache_bytes is constant across sizes, so
 *          the frame count is `budget / block` (min 1) and the bucket count
 *          is the next power of two above twice the frames (min
 *          ::k_cbs_bucket_min). The backend is wired in through the
 *          vsource-seam meter so `meter.calls` counts storage commands.
 *
 * @param[out] c           Cache bundle to populate.
 * @param[in]  be          Backend with a live `read` (post-setup).
 * @param[in]  blob_bytes  Object length in bytes.
 * @param[in]  block_bytes Swept frame size in bytes.
 *
 * @return int 0 on success, 1 on allocation or init failure.
 *
 * @pre `be->setup` succeeded for this block size.
 * @pre @p c is writable.
 * @post On 0, `c->vm` is cold and ready for ::ra8_vmem_get.
 * @post On 1, everything partially acquired is freed.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static int
cbs_cache_open(cbs_cache_t* c, cbs_backend_t* be, uint32_t blob_bytes, uint32_t block_bytes)
{
  if ((c == nullptr) || (be == nullptr) || (be->read == nullptr) || (block_bytes == 0U)) {
    return 1;
  }
  *c        = (cbs_cache_t){};
  c->frames = (uint32_t)k_cbs_cache_bytes / block_bytes;
  if (c->frames == 0U) {
    c->frames = 1U;
  }
  uint32_t nbuckets = cbs_pow2_ceil(c->frames * 2U);
  if (nbuckets < (uint32_t)k_cbs_bucket_min) {
    nbuckets = (uint32_t)k_cbs_bucket_min;
  }
  c->frame_mem = (uint8_t*)malloc((size_t)c->frames * (size_t)block_bytes);
  c->meta      = (ra8_vmem_frame_t*)calloc((size_t)c->frames, sizeof(ra8_vmem_frame_t));
  c->buckets   = (int32_t*)malloc((size_t)nbuckets * sizeof(int32_t));
  if ((c->frame_mem == nullptr) || (c->meta == nullptr) || (c->buckets == nullptr)) {
    cbs_cache_close(c);
    return 1;
  }
  c->meter = (cbs_meter_t){.inner = be->read, .inner_ctx = be->read_ctx};
  if (ra8_vsource_init(&c->vs, c->objs, 1U) != k_ra8_ok) {
    cbs_cache_close(c);
    return 1;
  }
  if (ra8_vsource_add_paged(&c->vs, cbs_meter_read, &c->meter, 0U, blob_bytes, &c->object_id) !=
      k_ra8_ok) {
    cbs_cache_close(c);
    return 1;
  }
  const ra8_vmem_cfg_t cfg = {.frame_mem    = c->frame_mem,
                              .frame_bytes  = block_bytes,
                              .frame_count  = c->frames,
                              .meta         = c->meta,
                              .buckets      = c->buckets,
                              .bucket_count = nbuckets,
                              .loader       = ra8_vsource_loader,
                              .loader_ctx   = &c->vs};
  if (ra8_vmem_init(&c->vm, &cfg) != k_ra8_ok) {
    cbs_cache_close(c);
    return 1;
  }
  return 0;
}

/**
 * @brief Drive one timed leg: @p n_reads requests of ::k_cbs_req_bytes each,
 *        at offsets `(i * req) % wrap_bytes`, verifying every byte returned.
 *
 * @details `wrap_bytes == blob_bytes` yields sequential whole-object passes
 *          (leg a); `wrap_bytes == block_bytes` pins the loop inside block 0
 *          (leg b, the pure hit path after one cold fill). The wall clock
 *          brackets only this loop -- backend setup and container packing
 *          are never timed.
 *
 * @param[in,out] c          An open, cold ::cbs_cache_t.
 * @param[in]     blob       Ground-truth payload for verification.
 * @param[in]     n_reads    Requests to issue.
 * @param[in]     wrap_bytes Offset wrap span (see details).
 * @param[out]    row        Row receiving reads / stats / wall time.
 *
 * @return int 0 on success, 1 on any get/put/verify failure.
 *
 * @pre @p c was opened by ::cbs_cache_open and is unused (cold).
 * @pre @p wrap_bytes is a non-zero multiple of ::k_cbs_req_bytes.
 * @post `row->wall_ns`, `row->reads`, and the hit/miss/eviction counters are
 *       filled from the cache's own accounting.
 * @post The cache holds no pins (every get was put back).
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static int cbs_drive(cbs_cache_t*   c,
                     const uint8_t* blob,
                     uint64_t       n_reads,
                     uint64_t       wrap_bytes,
                     cbs_row_t*     row)
{
  if ((c == nullptr) || (blob == nullptr) || (row == nullptr) || (wrap_bytes == 0U)) {
    return 1;
  }
  const uint64_t req   = (uint64_t)k_cbs_req_bytes;
  const uint64_t block = (uint64_t)c->vm.cfg.frame_bytes;
  uint64_t       bad   = 0U;
  const uint64_t t0    = cbs_now_ns();
  for (uint64_t i = 0U; i < n_reads; ++i) {
    const uint64_t off  = (i * req) % wrap_bytes;
    void*          page = nullptr;
    if (ra8_vmem_get(&c->vm, c->object_id, off, &page) != k_ra8_ok) {
      bad++;
      continue;
    }
    const uint8_t* piece = &((const uint8_t*)page)[off % block];
    if (memcmp(piece, &blob[off], (size_t)req) != 0) {
      bad++;
    }
    if (ra8_vmem_put(&c->vm, page) != k_ra8_ok) {
      bad++;
    }
  }
  row->wall_ns  = cbs_now_ns() - t0;
  row->reads    = n_reads;
  uint32_t hits = 0U;
  uint32_t miss = 0U;
  uint32_t evic = 0U;
  if (ra8_vmem_stats(&c->vm, &hits, &miss, &evic) != k_ra8_ok) {
    return 1;
  }
  row->hits      = (uint64_t)hits;
  row->misses    = (uint64_t)miss;
  row->evictions = (uint64_t)evic;
  if (bad != 0U) {
    (void)fprintf(stderr, "sweep-block: %llu bad accesses\n", (unsigned long long)bad);
    return 1;
  }
  return 0;
}

/** @brief Leg indices for ::cbs_run_block's per-leg loop. */
typedef enum : uint8_t {
  k_cbs_leg_seq   = 0U, /**< Sequential whole-object scan. */
  k_cbs_leg_hot   = 1U, /**< Same-block re-read loop.      */
  k_cbs_leg_count = 2U, /**< Number of legs.               */
} cbs_leg_t;

/**
 * @var s_cbs_leg_names
 * @brief Report names for the two workload legs, indexed by ::cbs_leg_t.
 * @details Read-only.
 * @note Only the run/report functions read this.
 * @since 0.1.0
 */
static const char* const s_cbs_leg_names[k_cbs_leg_count] = {"seq", "hot"};

/** @brief Print one machine-parseable result row (`sweep-block key=value ...`). */
static void cbs_print_row(const cbs_row_t* r)
{
  if ((r == nullptr) || (r->reads == 0U)) {
    return;
  }
  const uint64_t wall = (r->wall_ns == 0U) ? 1U : r->wall_ns;
  const double   mib_s =
    ((double)r->reads * (double)k_cbs_req_bytes / s_cbs_mib_f) / ((double)wall / s_cbs_ns_per_s_f);
  (void)printf("sweep-block backend=%s leg=%s block=%u frames=%u reads=%llu hits=%llu "
               "misses=%llu evictions=%llu backend_calls=%llu backend_bytes=%llu "
               "src_bytes=%llu backing_bytes=%llu wall_us=%llu mib_s=%.1f ns_per_read=%.1f\n",
               r->backend,
               r->leg,
               r->block_bytes,
               r->frames,
               (unsigned long long)r->reads,
               (unsigned long long)r->hits,
               (unsigned long long)r->misses,
               (unsigned long long)r->evictions,
               (unsigned long long)r->be_calls,
               (unsigned long long)r->be_bytes,
               (unsigned long long)r->src_bytes,
               (unsigned long long)r->backing_bytes,
               (unsigned long long)(wall / (uint64_t)k_cbs_ns_per_us),
               mib_s,
               (double)wall / (double)r->reads);
}

/**
 * @brief Run both legs of one (backend, block size) combination.
 *
 * @details Sets the backend up once for the size, then runs each leg on a
 *          fresh cold cache; per-leg backend counters are taken as deltas so
 *          open-time header/table reads never pollute a leg's traffic
 *          numbers. Each finished row is appended and printed immediately.
 *
 * @param[in,out] be          Backend to exercise.
 * @param[in]     blob        Source payload (ground truth).
 * @param[in]     block_bytes Swept block size.
 * @param[in,out] rows        Row array (capacity ::k_cbs_max_rows).
 * @param[in,out] nrows       Row count; incremented per finished leg.
 *
 * @return int 0 on success, 1 on any setup / leg failure.
 *
 * @pre @p rows has space for ::k_cbs_leg_count more rows.
 * @pre @p be has `setup` and `teardown` bound.
 * @post The backend is torn down (buffers freed) on every path.
 * @post On 0, ::k_cbs_leg_count rows were appended and printed.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static int cbs_run_block(cbs_backend_t* be,
                         const uint8_t* blob,
                         uint32_t       block_bytes,
                         cbs_row_t*     rows,
                         uint32_t*      nrows)
{
  if ((be == nullptr) || (blob == nullptr) || (rows == nullptr) || (nrows == nullptr)) {
    return 1;
  }
  if (be->setup(be, blob, (uint32_t)k_cbs_blob_bytes, block_bytes) != 0) {
    return 1;
  }
  int rc = 0;
  for (uint8_t leg = 0U; leg < (uint8_t)k_cbs_leg_count; ++leg) {
    cbs_cache_t c = {};
    if (cbs_cache_open(&c, be, (uint32_t)k_cbs_blob_bytes, block_bytes) != 0) {
      rc = 1;
      break;
    }
    const uint64_t src0 = (be->src_bytes != nullptr) ? *be->src_bytes : 0U;
    const bool     seq  = (leg == (uint8_t)k_cbs_leg_seq);
    const uint64_t n =
      seq ? ((uint64_t)k_cbs_seq_passes * (uint64_t)k_cbs_blob_bytes / (uint64_t)k_cbs_req_bytes)
          : (uint64_t)k_cbs_hot_reads;
    const uint64_t wrap = seq ? (uint64_t)k_cbs_blob_bytes : (uint64_t)block_bytes;
    cbs_row_t*     row  = &rows[*nrows];
    *row                = (cbs_row_t){.backend       = be->name,
                                      .leg           = s_cbs_leg_names[leg],
                                      .block_bytes   = block_bytes,
                                      .frames        = c.frames,
                                      .backing_bytes = be->backing_bytes};
    const int drc       = cbs_drive(&c, blob, n, wrap, row);
    row->be_calls       = c.meter.calls;
    row->be_bytes       = c.meter.bytes;
    row->src_bytes      = (be->src_bytes != nullptr) ? (*be->src_bytes - src0) : c.meter.bytes;
    cbs_cache_close(&c);
    if (drc != 0) {
      rc = 1;
      break;
    }
    (*nrows)++;
    cbs_print_row(row);
  }
  be->teardown(be);
  return rc;
}

/* ------------------------------------------------------------ reporting -- */

/** @brief Format a block size as a short label ("512B", "64K") into @p out. */
static void cbs_block_label(uint32_t block, char* out, size_t cap)
{
  if ((out == nullptr) || (cap == 0U)) {
    return;
  }
  if (block >= (uint32_t)k_cbs_kib) {
    (void)snprintf(out, cap, "%uK", block / (uint32_t)k_cbs_kib);
  } else {
    (void)snprintf(out, cap, "%uB", block);
  }
}

/** @brief Find the row for (backend, leg, block), or NULL. */
static const cbs_row_t*
cbs_find_row(const cbs_row_t* rows, uint32_t n, const char* be, const char* leg, uint32_t block)
{
  if ((rows == nullptr) || (be == nullptr) || (leg == nullptr)) {
    return nullptr;
  }
  for (uint32_t i = 0U; i < n; ++i) {
    if ((rows[i].block_bytes == block) && (strcmp(rows[i].backend, be) == 0) &&
        (strcmp(rows[i].leg, leg) == 0)) {
      return &rows[i];
    }
  }
  return nullptr;
}

/** @brief Delivered-payload throughput of a row in MiB/s. */
static double cbs_row_mibs(const cbs_row_t* r)
{
  if ((r == nullptr) || (r->reads == 0U)) {
    return 0.0;
  }
  const uint64_t wall = (r->wall_ns == 0U) ? 1U : r->wall_ns;
  return ((double)r->reads * (double)k_cbs_req_bytes / s_cbs_mib_f) /
         ((double)wall / s_cbs_ns_per_s_f);
}

/**
 * @brief Print one backend's sequential-scan summary table (leg a).
 *
 * @details Derives an estimated per-miss fill cost by subtracting the
 *          independently measured hit-path cost (the hot leg's ns/read at
 *          the same size) from the sequential wall time and dividing the
 *          remainder across the misses -- the Ch 23.2 method of separating
 *          per-request overhead from streaming cost.
 *
 * @param[in] rows    All finished rows.
 * @param[in] nrows   Number of rows.
 * @param[in] be      Backend name to summarise.
 * @param[in] blocks  The swept sizes, ascending.
 * @param[in] nblocks Number of swept sizes.
 *
 * @return void.
 *
 * @pre Rows for @p be exist for both legs at each size (partial rows skip).
 * @pre Stdout is writable.
 * @post One markdown table for @p be was printed.
 * @post No row data is modified.
 *
 * @note Not thread-safe (stdout).
 * @since 0.1.0
 */
static void cbs_print_seq_table(const cbs_row_t* rows,
                                uint32_t         nrows,
                                const char*      be,
                                const uint32_t*  blocks,
                                uint32_t         nblocks)
{
  (void)printf("\n### `%s` -- sequential whole-object scan (leg a)\n\n", be);
  (void)printf("| block | frames | MiB/s | ns/read | est us/miss | backend calls | src MiB | "
               "backing MiB |\n");
  (void)printf("|------:|-------:|------:|--------:|------------:|--------------:|--------:|"
               "------------:|\n");
  for (uint32_t i = 0U; i < nblocks; ++i) {
    const cbs_row_t* s = cbs_find_row(rows, nrows, be, "seq", blocks[i]);
    const cbs_row_t* h = cbs_find_row(rows, nrows, be, "hot", blocks[i]);
    if ((s == nullptr) || (h == nullptr) || (s->reads == 0U) || (h->reads == 0U)) {
      continue;
    }
    const double hit_ns  = (double)h->wall_ns / (double)h->reads;
    double       miss_ns = (double)s->wall_ns - (hit_ns * (double)s->reads);
    miss_ns              = (s->misses == 0U) ? 0.0 : (miss_ns / (double)s->misses);
    if (miss_ns < 0.0) {
      miss_ns = 0.0;
    }
    char label[16] = {};
    cbs_block_label(blocks[i], label, sizeof(label));
    (void)printf("| %5s | %6u | %5.0f | %7.1f | %11.1f | %13llu | %7.2f | %11.2f |\n",
                 label,
                 s->frames,
                 cbs_row_mibs(s),
                 (double)s->wall_ns / (double)s->reads,
                 miss_ns / s_cbs_ns_per_us_f,
                 (unsigned long long)s->be_calls,
                 (double)s->src_bytes / s_cbs_mib_f,
                 (double)s->backing_bytes / s_cbs_mib_f);
  }
}

/**
 * @brief Print one backend's same-block re-read summary table (leg b).
 *
 * @details Shows the pure cache-hit path per block size; a flat ns/read
 *          column is the expected result (hit cost independent of the block
 *          size), with exactly one cold miss per row.
 *
 * @param[in] rows    All finished rows.
 * @param[in] nrows   Number of rows.
 * @param[in] be      Backend name to summarise.
 * @param[in] blocks  The swept sizes, ascending.
 * @param[in] nblocks Number of swept sizes.
 *
 * @return void.
 *
 * @pre Hot rows for @p be exist at each size (missing rows are skipped).
 * @pre Stdout is writable.
 * @post One markdown table for @p be was printed.
 * @post No row data is modified.
 *
 * @note Not thread-safe (stdout).
 * @since 0.1.0
 */
static void cbs_print_hot_table(const cbs_row_t* rows,
                                uint32_t         nrows,
                                const char*      be,
                                const uint32_t*  blocks,
                                uint32_t         nblocks)
{
  (void)printf("\n### `%s` -- same-block re-read (leg b, pure hit path)\n\n", be);
  (void)printf("| block | ns/read | hits | misses |\n");
  (void)printf("|------:|--------:|-----:|-------:|\n");
  for (uint32_t i = 0U; i < nblocks; ++i) {
    const cbs_row_t* h = cbs_find_row(rows, nrows, be, "hot", blocks[i]);
    if ((h == nullptr) || (h->reads == 0U)) {
      continue;
    }
    char label[16] = {};
    cbs_block_label(blocks[i], label, sizeof(label));
    (void)printf("| %5s | %7.1f | %llu | %6llu |\n",
                 label,
                 (double)h->wall_ns / (double)h->reads,
                 (unsigned long long)h->hits,
                 (unsigned long long)h->misses);
  }
}

/**
 * @struct cbs_knee_t
 * @brief The computed crossover of the chunked backend's sequential sweep.
 * @details Filled by ::cbs_find_knee; `knee_block` is the smallest size whose
 *          throughput reaches ::k_cbs_knee_pct percent of `peak_mibs`.
 * @invariant Both `knee_block` and `peak_block` always name swept sizes.
 * @since 0.1.0
 */
typedef struct {
  double   peak_mibs;  /**< Best sequential throughput seen.   */
  uint32_t peak_block; /**< Size achieving the peak.           */
  double   knee_mibs;  /**< Throughput at the knee.            */
  uint32_t knee_block; /**< Smallest size within the knee bar. */
} cbs_knee_t;

/**
 * @brief Locate the rbkc-z9 sequential throughput peak and knee.
 *
 * @param[in]  rows    All finished rows.
 * @param[in]  nrows   Number of rows.
 * @param[in]  blocks  The swept sizes, ascending.
 * @param[in]  nblocks Number of swept sizes.
 * @param[out] out     Receives the peak + knee.
 *
 * @return bool true when rbkc-z9 sequential rows existed, false otherwise.
 * @retval true  @p out is fully populated.
 * @retval false No usable rows; @p out is zeroed.
 *
 * @pre @p blocks is sorted ascending (the first size over the bar wins).
 * @pre @p out is writable.
 * @post On true, `out->knee_mibs >= k_cbs_knee_pct% * out->peak_mibs`.
 * @post No row data is modified.
 *
 * @note Thread-safe over quiescent rows (pure read).
 * @since 0.1.0
 */
static bool cbs_find_knee(const cbs_row_t* rows,
                          uint32_t         nrows,
                          const uint32_t*  blocks,
                          uint32_t         nblocks,
                          cbs_knee_t*      out)
{
  if ((out == nullptr) || (blocks == nullptr)) {
    return false;
  }
  *out = (cbs_knee_t){};
  for (uint32_t i = 0U; i < nblocks; ++i) {
    const cbs_row_t* s = cbs_find_row(rows, nrows, "rbkc-z9", "seq", blocks[i]);
    const double     m = cbs_row_mibs(s);
    if (m > out->peak_mibs) {
      out->peak_mibs  = m;
      out->peak_block = blocks[i];
    }
  }
  if (out->peak_mibs <= 0.0) {
    return false;
  }
  const double bar = out->peak_mibs * ((double)k_cbs_knee_pct / s_cbs_pct_f);
  out->knee_block  = out->peak_block;
  out->knee_mibs   = out->peak_mibs;
  for (uint32_t i = 0U; i < nblocks; ++i) {
    const cbs_row_t* s = cbs_find_row(rows, nrows, "rbkc-z9", "seq", blocks[i]);
    const double     m = cbs_row_mibs(s);
    if (m >= bar) {
      out->knee_block = blocks[i];
      out->knee_mibs  = m;
      break;
    }
  }
  return true;
}

/**
 * @brief Name the measured crossover and print the chunk-size recommendation.
 *
 * @details The knee is the smallest block size whose sequential throughput on
 *          the chunked (rbkc-z9) backend reaches ::k_cbs_knee_pct percent of
 *          the peak across all sizes -- i.e. where per-request overhead has
 *          stopped dominating (Memory Systems Ch 23.2.1). The verdict names
 *          the knee against the current 64 KiB `.rabook` default and states
 *          the host-measurement caveat for the SD hardware leg.
 *
 * @param[in] rows    All finished rows.
 * @param[in] nrows   Number of rows.
 * @param[in] blocks  The swept sizes, ascending.
 * @param[in] nblocks Number of swept sizes.
 *
 * @return void.
 *
 * @pre rbkc-z9 seq rows exist (the function prints nothing useful otherwise).
 * @pre @p blocks is sorted ascending.
 * @post The crossover paragraph was printed to stdout.
 * @post No row data is modified.
 *
 * @note Not thread-safe (stdout).
 * @since 0.1.0
 */
static void
cbs_print_crossover(const cbs_row_t* rows, uint32_t nrows, const uint32_t* blocks, uint32_t nblocks)
{
  cbs_knee_t k = {};
  if (!cbs_find_knee(rows, nrows, blocks, nblocks, &k)) {
    (void)printf("\n(no rbkc-z9 sequential rows; crossover not computed)\n");
    return;
  }
  const double knee_pct   = s_cbs_pct_f * k.knee_mibs / k.peak_mibs;
  char         peak_l[16] = {};
  char         knee_l[16] = {};
  char         def_l[16]  = {};
  cbs_block_label(k.peak_block, peak_l, sizeof(peak_l));
  cbs_block_label(k.knee_block, knee_l, sizeof(knee_l));
  cbs_block_label((uint32_t)k_cbs_default_chunk, def_l, sizeof(def_l));
  (void)printf("\n### Measured crossover (rbkc-z9, sequential)\n\n");
  (void)printf("Peak %.0f MiB/s at %s; knee (first size within %u%% of peak) at %s "
               "(%.0f MiB/s, %.1f%% of peak).\n",
               k.peak_mibs,
               peak_l,
               (unsigned)k_cbs_knee_pct,
               knee_l,
               k.knee_mibs,
               knee_pct);
  if (k.knee_block == (uint32_t)k_cbs_default_chunk) {
    (void)printf("The measured knee lands on the current %s `.rabook` chunk default (#204): "
                 "keep it.\n",
                 def_l);
  } else if (k.knee_block < (uint32_t)k_cbs_default_chunk) {
    (void)printf("The knee sits BELOW the current %s default: %s already reaches %.1f%% of "
                 "peak on this backend, at a smaller per-miss inflate latency and less RAM "
                 "per frame.\n",
                 def_l,
                 knee_l,
                 knee_pct);
  } else {
    (void)printf("The knee sits ABOVE the current %s default: sequential throughput is still "
                 "climbing past it; consider a larger chunk if per-miss latency allows.\n",
                 def_l);
  }
  (void)printf("Caveat: these are host numbers -- per-request cost here is only the chunk "
               "lookup + zlib stream setup. SD-over-SPI adds real per-command overhead "
               "(CMD17 loops, #202), which pushes the knee toward larger blocks; the "
               "hardware leg of #208 must re-run this sweep on the bench before shrinking "
               "the chunk size below the default.\n");
}

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

/**
 * @var s_cbs_blocks
 * @brief The swept block sizes, ascending (drives loops + knee search).
 * @details Values come from ::cbs_block_size_t.
 * @note Read-only.
 * @since 0.1.0
 */
static const uint32_t s_cbs_blocks[] = {
  (uint32_t)k_cbs_block_512b,
  (uint32_t)k_cbs_block_1kib,
  (uint32_t)k_cbs_block_4kib,
  (uint32_t)k_cbs_block_16kib,
  (uint32_t)k_cbs_block_64kib,
  (uint32_t)k_cbs_block_256kib,
};

int cb_sweep_block(void)
{
  uint8_t* blob = (uint8_t*)malloc((size_t)k_cbs_blob_bytes);
  if (blob == nullptr) {
    (void)fprintf(stderr, "sweep-block: payload allocation failed\n");
    return 1;
  }
  cbs_fill_text(blob, (uint32_t)k_cbs_blob_bytes);

  (void)printf("# #208 block/frame-size sweep\n\n");
  (void)printf("payload=%u cache_budget=%u req=%u seq_passes=%u hot_reads=%u zlib_level=%d "
               "cache=ra8_vmem(SLRU)\n\n",
               (unsigned)k_cbs_blob_bytes,
               (unsigned)k_cbs_cache_bytes,
               (unsigned)k_cbs_req_bytes,
               (unsigned)k_cbs_seq_passes,
               (unsigned)k_cbs_hot_reads,
               MZ_BEST_COMPRESSION);

  static cbs_row_t rows[k_cbs_max_rows] = {};
  uint32_t         nrows                = 0U;
  int              rc                   = 0;
  const uint32_t   nbe     = (uint32_t)(sizeof(s_cbs_backends) / sizeof(s_cbs_backends[0]));
  const uint32_t   nblocks = (uint32_t)(sizeof(s_cbs_blocks) / sizeof(s_cbs_blocks[0]));
  for (uint32_t b = 0U; (b < nbe) && (rc == 0); ++b) {
    for (uint32_t s = 0U; (s < nblocks) && (rc == 0); ++s) {
      rc = cbs_run_block(&s_cbs_backends[b], blob, s_cbs_blocks[s], rows, &nrows);
    }
  }
  if (rc == 0) {
    (void)printf("\n## Summary (payload 8 MiB, cache budget 1 MiB, %u B reader requests)\n",
                 (unsigned)k_cbs_req_bytes);
    for (uint32_t b = 0U; b < nbe; ++b) {
      cbs_print_seq_table(rows, nrows, s_cbs_backends[b].name, s_cbs_blocks, nblocks);
      cbs_print_hot_table(rows, nrows, s_cbs_backends[b].name, s_cbs_blocks, nblocks);
    }
    cbs_print_crossover(rows, nrows, s_cbs_blocks, nblocks);
  } else {
    (void)fprintf(stderr, "sweep-block: sweep aborted\n");
  }
  free(blob);
  return rc;
}
