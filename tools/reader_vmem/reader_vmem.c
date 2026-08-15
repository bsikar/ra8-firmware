/**
 * @file reader_vmem.c
 * @brief Drive the real virtual-memory cache with a deterministic reader trace
 *
 * @details
 * This host decision-record tool registers a synthetic book through the real
 * `ra8_vsource` callback seam, drives the production `ra8_vmem` SLRU cache, and
 * emits one cache_bench-compatible `<object> <frame>` reference per access.
 * Synthetic source bytes are generated at the requested offset, so the source
 * exercises the complete miss/load path without allocating an 8 MiB replica.
 * Cache RAM is carved from the explicit composition-root workspace below.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_log.h"
#include "ra8_vmem.h"
#include "ra8_vsource.h"
#include "reader_vmem_internal.h"

/** @brief Book and workload model dimensions. */
typedef enum : uint32_t {
  k_rv_header_frames  = 4U,    /**< Persistent header frames.     */
  k_rv_chapters       = 24U,   /**< Synthetic chapter count.      */
  k_rv_chap_min_fr    = 8U,    /**< Minimum frames per chapter.   */
  k_rv_chap_span_fr   = 160U,  /**< Random chapter-size span.     */
  k_rv_backglance_pct = 12U,   /**< Prior-page glance percent.    */
  k_rv_toc_rounds     = 600U,  /**< TOC jump rounds.              */
  k_rv_toc_read_fr    = 6U,    /**< Frames read after a TOC jump. */
  k_rv_sr_rounds      = 40U,   /**< Scan-resistance rounds.       */
  k_rv_sr_hot_pass    = 3U,    /**< Hot-set passes per round.     */
  k_rv_sr_hot_fr      = 200U,  /**< Hot-set frame count.          */
  k_rv_sr_scan_fr     = 1200U, /**< Scan frames per round.        */
  k_rv_pct_base       = 100U,  /**< Percentage denominator.       */
  k_rv_def_budget     = 256U,  /**< Default cache-frame budget.   */
  k_rv_max_objs       = 2U,    /**< Source registry capacity.     */
} rv_dimension_t;

/** @brief Deterministic generation and decimal constants. */
typedef enum : uint32_t {
  k_rv_xs_shift_a     = 13U,         /**< First xorshift distance.      */
  k_rv_xs_shift_b     = 7U,          /**< Second xorshift distance.     */
  k_rv_xs_shift_c     = 17U,         /**< Third xorshift distance.      */
  k_rv_fill_mul       = 2654435761U, /**< Synthetic-byte multiplier.    */
  k_rv_fill_shift     = 24U,         /**< Synthetic-byte shift.         */
  k_rv_decimal_base   = 10U,         /**< Decimal radix.                */
  k_rv_rate_scale     = 10000U,      /**< Hundredths-percent scale.     */
  k_rv_rate_digits    = 2U,          /**< Printed fractional digits.    */
  k_rv_workspace_pads = 3U,          /**< Maximum region padding count. */
} rv_constant_t;

/** @brief Fixed deterministic RNG seed. */
typedef enum : uint64_t {
  k_rv_rng_seed = 0x9E3779B97F4A7C15ULL, /**< Reproducible xorshift seed. */
} rv_seed_t;

/** @brief Compile-time bytes reserved by the standalone composition root. */
typedef enum : uint64_t {
  k_rv_workspace_storage_bytes =
    ((uint64_t)READER_VMEM_MAX_BUDGET *
     ((uint64_t)k_rv_frame_bytes + sizeof(ra8_vmem_frame_t) + sizeof(ra8_vmem_key_t))) +
    ((uint64_t)k_rv_bucket_count * sizeof(int32_t)) +
    ((uint64_t)k_rv_workspace_pads * (_Alignof(max_align_t) - 1U)), /**< BSS bytes. */
} rv_storage_size_t;

static_assert(READER_VMEM_MAX_BUDGET > 0U);
static_assert(k_rv_workspace_storage_bytes <= SIZE_MAX);

/** @brief Explicit aligned storage owned only by this single-process CLI. */
typedef union {
  max_align_t alignment;                           /**< Enforces region alignment. */
  uint8_t     bytes[k_rv_workspace_storage_bytes]; /**< Cache workspace backing.   */
} rv_workspace_storage_t;

/**
 * @brief Composition-root BSS cache memory, never used by reusable libraries.
 * @details The workspace planner accepts arbitrary caller backings; this one
 *          named instance is the standalone tool's explicit RAM budget.
 */
static rv_workspace_storage_t s_reader_workspace;

/** @brief Per-chapter frame extent. */
typedef struct {
  uint32_t first; /**< First frame. */
  uint32_t count; /**< Frame count. */
} rv_chapter_t;

/** @brief Statelessly generated book extent. */
typedef struct {
  uint64_t bytes; /**< Total addressable synthetic bytes. */
} rv_book_t;

/** @brief Workload state shared by all navigation phases. */
typedef struct {
  rv_chapter_t chapters[k_rv_chapters]; /**< Deterministic chapter extents. */
  uint32_t     total_frames;            /**< Total synthetic book frames.   */
  uint64_t     rng;                     /**< Xorshift state.                */
  ra8_vmem_t*  vm;                      /**< Production cache instance.     */
  rv_trace_t*  trace;                   /**< Atomic raw-fd trace sink.      */
  uint32_t     object_id;               /**< Registered book object.        */
  uint64_t     accesses;                /**< References attempted.          */
  bool         failed;                  /**< Sticky cache/trace failure.    */
} rv_driver_t;

void ra8_log_emit_error(const char* tag, const char* message)
{
  priv_rv_diag("[ra8_log] ");
  priv_rv_diag(tag);
  priv_rv_diag(": ");
  priv_rv_diag(message);
  priv_rv_diag("\n");
}

void ra8_log_emit_error_val(const char* tag, const char* message, uint32_t value)
{
  priv_rv_diag("[ra8_log] ");
  priv_rv_diag(tag);
  priv_rv_diag(": ");
  priv_rv_diag(message);
  priv_rv_diag(" =");
  priv_rv_diag_u64(value);
  priv_rv_diag("\n");
}

/**
 * @brief Advance the driver's fixed-seed xorshift generator.
 * @details Applies the fixed three-shift recurrence used by every workload phase.
 * @param[in,out] state Non-zero deterministic generator state.
 * @return Newly advanced generator value.
 * @retval value Exact next state after one recurrence.
 * @pre @p state is non-null and writable.
 * @pre The caller initialized state from the documented fixed seed.
 * @post `*state` equals the returned value.
 * @post Exactly one generator step occurred.
 * @note Not thread-safe through a shared state word.
 * @since 0.1.0
 */
RA8_INTERNAL static uint64_t internal_rng(uint64_t* state)
{
  uint64_t value = *state;
  value ^= value << (uint32_t)k_rv_xs_shift_a;
  value ^= value >> (uint32_t)k_rv_xs_shift_b;
  value ^= value << (uint32_t)k_rv_xs_shift_c;
  *state = value;
  return value;
}

/**
 * @brief Draw one deterministic value below a non-negative span.
 * @details Advances the generator for non-zero spans and maps by unsigned modulo.
 * @param[in,out] state Deterministic generator state.
 * @param[in] span Exclusive upper bound; zero selects deterministic zero.
 * @return Value in `[0, span)`, or zero when @p span is zero.
 * @retval 0 The zero-span result or a valid sampled zero.
 * @retval sampled A value strictly below non-zero @p span.
 * @pre @p state is non-null and writable.
 * @pre @p span may be any `uint32_t` value.
 * @post Non-zero span advances state exactly once.
 * @post Zero span leaves state unchanged.
 * @note Modulo bias is intentional for deterministic workload generation.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_below(uint64_t* state, uint32_t span)
{
  return (span == 0U) ? 0U : (uint32_t)(internal_rng(state) % (uint64_t)span);
}

/**
 * @brief Generate exact synthetic source bytes directly at the read seam.
 * @details Computes each byte from its absolute offset, avoiding a synthetic-book allocation.
 * @param[in] context Bound ::rv_book_t extent.
 * @param[in] offset First requested byte offset.
 * @param[out] buffer Destination spanning @p length bytes.
 * @param[in] length Exact requested byte count.
 * @return Canonical virtual-source status.
 * @retval k_ra8_ok The complete range was generated.
 * @retval k_ra8_err_null_ptr Context or destination was null.
 * @retval k_ra8_err_out_of_range Request exceeds the synthetic book.
 * @pre @p context and @p buffer are null or valid for their declared extents.
 * @pre The book byte extent was derived from complete frame geometry.
 * @post Success initializes exactly @p length destination bytes.
 * @post Failure does not read or write outside caller storage.
 * @note Pure for a fixed context and thread-safe for independent buffers.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_book_read(void* context, uint64_t offset, uint8_t* buffer, uint32_t length)
{
  const rv_book_t* book = (const rv_book_t*)context;
  if (book == nullptr || buffer == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (offset > book->bytes || (uint64_t)length > (book->bytes - offset)) {
    return k_ra8_err_out_of_range;
  }
  for (uint32_t i = 0U; i < length; ++i) {
    const uint64_t absolute = offset + i;
    buffer[i] = (uint8_t)((absolute * (uint64_t)k_rv_fill_mul) >> (uint32_t)k_rv_fill_shift);
  }
  return k_ra8_ok;
}

/**
 * @brief Lay out deterministic variable-size chapters.
 * @details Consumes one bounded random draw per chapter after the fixed header extent.
 * @param[in,out] driver Workload driver owning RNG and chapter table.
 * @pre @p driver is non-null and its RNG was initialized.
 * @pre Chapter constants cannot overflow the `uint32_t` frame cursor.
 * @post Every chapter extent is initialized, ordered, and non-overlapping.
 * @post `total_frames` is one past the final chapter frame.
 * @note Not thread-safe through @p driver.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_layout_book(rv_driver_t* driver)
{
  uint32_t cursor = (uint32_t)k_rv_header_frames;
  for (uint32_t chapter = 0U; chapter < (uint32_t)k_rv_chapters; ++chapter) {
    const uint32_t count =
      (uint32_t)k_rv_chap_min_fr + internal_below(&driver->rng, (uint32_t)k_rv_chap_span_fr);
    driver->chapters[chapter] = (rv_chapter_t){.first = cursor, .count = count};
    cursor += count;
  }
  driver->total_frames = cursor;
}

/**
 * @brief Access one production cache frame and append its trace record.
 * @details Exercises get/put through production vmem, records the same reference,
 * and makes any cache or trace failure sticky while preserving workload length.
 * @param[in,out] driver Fully prepared workload driver.
 * @param[in] frame Synthetic book frame number to reference.
 * @pre @p driver owns live cache and trace contexts.
 * @pre @p frame is below `driver->total_frames`.
 * @post Access count advances exactly once and one trace append is attempted.
 * @post Any cache/trace error leaves `driver->failed` set.
 * @note Not thread-safe through cache, trace, or driver state.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_touch(rv_driver_t* driver, uint32_t frame)
{
  const uint64_t  offset = (uint64_t)frame * (uint64_t)k_rv_frame_bytes;
  void*           page   = nullptr;
  const ra8_err_t get    = ra8_vmem_get(driver->vm, driver->object_id, offset, &page);
  if (get == k_ra8_ok) {
    if (ra8_vmem_put(driver->vm, page) != k_ra8_ok) {
      driver->failed = true;
    }
  } else {
    priv_rv_diag("vm_get(frame ");
    priv_rv_diag_u64(frame);
    priv_rv_diag(") failed: ");
    priv_rv_diag_u64(get);
    priv_rv_diag("\n");
    driver->failed = true;
  }
  if (!priv_rv_trace_reference(driver->trace, driver->object_id, frame)) {
    driver->failed = true;
  }
  ++driver->accesses;
}

/**
 * @brief Re-reference the hot header and TOC region.
 * @details Touches the fixed leading frame range in ascending order.
 * @param[in,out] driver Prepared workload driver.
 * @pre @p driver owns live cache and trace contexts.
 * @pre Synthetic book contains at least ::k_rv_header_frames frames.
 * @post Exactly ::k_rv_header_frames references were attempted.
 * @post Sticky failure semantics from ::internal_touch are preserved.
 * @note Not thread-safe through @p driver.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_touch_header(rv_driver_t* driver)
{
  for (uint32_t frame = 0U; frame < (uint32_t)k_rv_header_frames; ++frame) {
    internal_touch(driver, frame);
  }
}

/**
 * @brief Run linear page-turns with hot furniture and back-glances.
 * @details Visits every chapter page, refreshing header frames each turn and
 * adding deterministic prior-page glances according to the fixed percentage.
 * @param[in,out] driver Prepared and laid-out workload driver.
 * @pre @p driver owns live cache, trace, chapter, and RNG state.
 * @pre Chapter extents lie within `driver->total_frames`.
 * @post Every chapter page was referenced once in reading order.
 * @post RNG and access counters reflect all deterministic glance decisions.
 * @note Continues after sticky failure to preserve deterministic trace attempts.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_phase_linear(rv_driver_t* driver)
{
  for (uint32_t chapter = 0U; chapter < (uint32_t)k_rv_chapters; ++chapter) {
    const rv_chapter_t extent = driver->chapters[chapter];
    for (uint32_t index = 0U; index < extent.count; ++index) {
      const uint32_t frame = extent.first + index;
      internal_touch_header(driver);
      internal_touch(driver, frame);
      const bool glance =
        internal_below(&driver->rng, (uint32_t)k_rv_pct_base) < (uint32_t)k_rv_backglance_pct;
      if (glance && index > 0U) {
        internal_touch(driver, frame - 1U);
      }
    }
  }
}

/**
 * @brief Run TOC-driven chapter jumps.
 * @details Repeats header refresh, deterministic chapter selection, and a bounded prefix read.
 * @param[in,out] driver Prepared and laid-out workload driver.
 * @pre @p driver owns live cache, trace, chapter, and RNG state.
 * @pre Every chapter count is non-zero and within book geometry.
 * @post Exactly ::k_rv_toc_rounds chapter selections occurred.
 * @post Each selection touched no more than ::k_rv_toc_read_fr chapter frames.
 * @note Not thread-safe through @p driver.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_phase_toc(rv_driver_t* driver)
{
  for (uint32_t round = 0U; round < (uint32_t)k_rv_toc_rounds; ++round) {
    internal_touch_header(driver);
    const uint32_t     index  = internal_below(&driver->rng, (uint32_t)k_rv_chapters);
    const rv_chapter_t extent = driver->chapters[index];
    for (uint32_t page = 0U; page < (uint32_t)k_rv_toc_read_fr && page < extent.count; ++page) {
      internal_touch(driver, extent.first + page);
    }
  }
}

/**
 * @brief Run hot-set rereads interleaved with one-shot scan floods.
 * @details Alternates repeated hot prefixes with modular scans to exercise SLRU pollution resistance.
 * @param[in,out] driver Prepared and laid-out workload driver.
 * @pre @p driver owns live cache and trace state.
 * @pre Total book frames exceed the fixed header extent.
 * @post Every configured hot pass and scan reference was attempted.
 * @post Scan cursor advances deterministically by one scan extent per round.
 * @note Not thread-safe through @p driver.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_phase_scan(rv_driver_t* driver)
{
  uint32_t scan_at = (uint32_t)k_rv_header_frames + (uint32_t)k_rv_sr_hot_fr;
  for (uint32_t round = 0U; round < (uint32_t)k_rv_sr_rounds; ++round) {
    for (uint32_t pass = 0U; pass < (uint32_t)k_rv_sr_hot_pass; ++pass) {
      for (uint32_t hot = 0U; hot < (uint32_t)k_rv_sr_hot_fr; ++hot) {
        internal_touch(driver, (uint32_t)k_rv_header_frames + hot);
      }
    }
    for (uint32_t scan = 0U; scan < (uint32_t)k_rv_sr_scan_fr; ++scan) {
      const uint32_t span  = driver->total_frames - (uint32_t)k_rv_header_frames;
      const uint32_t frame = (uint32_t)k_rv_header_frames + ((scan_at + scan) % span);
      internal_touch(driver, frame);
    }
    scan_at += (uint32_t)k_rv_sr_scan_fr;
  }
}

/**
 * @brief Parse one strict non-zero decimal frame budget.
 * @details Accepts ASCII digits only and checks every base-ten accumulation for overflow.
 * @param[in] text NUL-terminated budget spelling.
 * @param[out] budget Receives the parsed non-zero frame count.
 * @return Whether the complete spelling is valid.
 * @retval true A non-zero `uint32_t` value was stored.
 * @retval false Input is null, empty, non-decimal, zero, or overflowing.
 * @pre @p text is null or NUL-terminated.
 * @pre @p budget is non-null and writable.
 * @post Success initializes @p budget exactly once.
 * @post Failure leaves @p budget unchanged.
 * @note Locale-independent and allocation-free.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_parse_budget(const char* text, uint32_t* budget)
{
  if (text == nullptr || text[0] == '\0') {
    return false;
  }
  uint64_t value = 0U;
  for (size_t i = 0U; text[i] != '\0'; ++i) {
    if (text[i] < '0' || text[i] > '9') {
      return false;
    }
    const uint32_t digit = (uint32_t)(text[i] - '0');
    if (value > ((UINT32_MAX - digit) / (uint32_t)k_rv_decimal_base)) {
      return false;
    }
    value = (value * (uint32_t)k_rv_decimal_base) + digit;
  }
  if (value == 0U) {
    return false;
  }
  *budget = (uint32_t)value;
  return true;
}

/**
 * @brief Initialise production vmem over already bound workspace views.
 * @details Binds explicit cache regions and the virtual-source loader into one configuration.
 * @param[out] vm Caller-owned cache object.
 * @param[in] workspace Bound non-overlapping cache regions.
 * @param[in] budget Number of frames represented by the regions.
 * @param[in,out] sources Initialized virtual-source registry.
 * @return Whether production cache initialization succeeded.
 * @retval true Cache is ready for get/put operations.
 * @retval false Production configuration validation failed.
 * @pre All pointers are non-null and workspace regions match @p budget.
 * @pre @p sources remains alive for the cache lifetime.
 * @post Success initializes @p vm without acquiring storage.
 * @post Failure leaves no hidden allocation or resource ownership.
 * @note Not thread-safe through @p vm or @p sources.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_vmem_setup(ra8_vmem_t*           vm,
                                             const rv_workspace_t* workspace,
                                             uint32_t              budget,
                                             ra8_vsource_t*        sources)
{
  const ra8_vmem_cfg_t config = {.frame_mem    = workspace->frame_mem,
                                 .frame_bytes  = (uint32_t)k_rv_frame_bytes,
                                 .frame_count  = budget,
                                 .meta         = workspace->meta,
                                 .keys         = workspace->keys,
                                 .buckets      = workspace->buckets,
                                 .bucket_count = (uint32_t)k_rv_bucket_count,
                                 .loader       = ra8_vsource_loader,
                                 .loader_ctx   = sources};
  return ra8_vmem_init(vm, &config) == k_ra8_ok;
}

/**
 * @brief Print an integer percentage rounded to two fractional digits.
 * @details Uses scaled integer arithmetic and raw diagnostic fragments only.
 * @param[in] hits Successful cache-hit count.
 * @param[in] accesses Total reference count.
 * @pre @p hits does not exceed the successful-access subset.
 * @pre Scaling `hits * 10000` is representable in `uint64_t`.
 * @post One decimal percentage spelling was attempted on standard error.
 * @post No cache, trace, or workload state changed.
 * @note Zero accesses render as `0.00`.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_report_rate(uint32_t hits, uint64_t accesses)
{
  uint64_t scaled = 0U;
  if (accesses != 0U) {
    scaled = (((uint64_t)hits * (uint64_t)k_rv_rate_scale) + (accesses / 2U)) / accesses;
  }
  priv_rv_diag_u64(scaled / (uint64_t)k_rv_pct_base);
  priv_rv_diag(".");
  const uint64_t fraction = scaled % (uint64_t)k_rv_pct_base;
  if (fraction < (uint64_t)k_rv_decimal_base) {
    priv_rv_diag("0");
  }
  priv_rv_diag_u64(fraction);
}

/**
 * @brief Report the firmware cache counters in the legacy CLI format.
 * @details Emits deterministic raw-descriptor fragments for golden-output parity.
 * @param[in] driver Completed workload driver.
 * @param[in] book Synthetic source extent.
 * @param[in] budget Configured frame budget.
 * @param[in] trace_path Published trace path.
 * @param[in] hits Production cache hit count.
 * @param[in] misses Production cache miss count.
 * @param[in] evictions Production cache eviction count.
 * @pre All pointers are non-null and NUL-terminated where applicable.
 * @pre Statistics were obtained from the completed cache instance.
 * @post Complete report fragments were attempted on standard error.
 * @post Input structures and published trace are unchanged.
 * @note Best-effort diagnostic failure does not alter process status.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_report(const rv_driver_t* driver,
                                         const rv_book_t*   book,
                                         uint32_t           budget,
                                         const char*        trace_path,
                                         uint32_t           hits,
                                         uint32_t           misses,
                                         uint32_t           evictions)
{
  priv_rv_diag("reader_vmem: book=");
  priv_rv_diag_u64(driver->total_frames);
  priv_rv_diag(" frames (");
  priv_rv_diag_u64(book->bytes);
  priv_rv_diag(" bytes), budget=");
  priv_rv_diag_u64(budget);
  priv_rv_diag(" frames\n  accesses=");
  priv_rv_diag_u64(driver->accesses);
  priv_rv_diag("  trace=");
  priv_rv_diag(trace_path);
  priv_rv_diag("\n  ra8_vmem SLRU: hits=");
  priv_rv_diag_u64(hits);
  priv_rv_diag(" misses=");
  priv_rv_diag_u64(misses);
  priv_rv_diag(" evictions=");
  priv_rv_diag_u64(evictions);
  priv_rv_diag("  hit_rate=");
  internal_report_rate(hits, driver->accesses);
  priv_rv_diag("%\n");
}

/**
 * @brief Report an exact compiled-workspace capacity failure.
 * @details Prints requested and compiled capacities using integer-only formatting.
 * @param[in] budget Requested frame budget.
 * @param[in] need Exact workspace requirement computed for that budget.
 * @pre @p need is non-null and fully initialized.
 * @pre Requirement exceeds either compiled frame or byte capacity.
 * @post One diagnostic message was attempted on standard error.
 * @post Workspace backing and requirement remain unchanged.
 * @note Best-effort output uses no stdio stream or allocation.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_report_workspace(uint32_t budget, const rv_workspace_need_t* need)
{
  priv_rv_diag("reader_vmem: budget ");
  priv_rv_diag_u64(budget);
  priv_rv_diag(" requires ");
  priv_rv_diag_u64(need->total_bytes);
  priv_rv_diag(" workspace bytes; compiled max budget ");
  priv_rv_diag_u64(READER_VMEM_MAX_BUDGET);
  priv_rv_diag(" provides ");
  priv_rv_diag_u64(sizeof(s_reader_workspace.bytes));
  priv_rv_diag(" bytes\n");
}

/**
 * @brief Bind the workspace and register the generated book source.
 * @details Derives exact region requirements, enforces the compiled ceiling,
 * binds typed views, and registers one stateless positioned-read source.
 * @param[in] budget Requested cache frame count.
 * @param[in] book Synthetic book extent.
 * @param[out] workspace Receives typed cache storage views.
 * @param[out] sources Receives initialized source registry.
 * @param[out] objects Caller-owned registry-object array.
 * @param[out] object_id Receives the registered synthetic object identifier.
 * @param[out] need Receives exact workspace geometry.
 * @return Whether all bounded composition steps succeeded.
 * @retval true Workspace and source are ready for cache initialization.
 * @retval false Requirement, capacity, binding, or registration failed.
 * @pre All pointers are non-null and object array has ::k_rv_max_objs entries.
 * @pre @p book has a non-zero bounded byte extent.
 * @post Success initializes every output without allocation.
 * @post Failure leaves no descriptor or dynamically owned storage.
 * @note The composition-root BSS backing supports this one CLI run.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_prepare(uint32_t             budget,
                                          const rv_book_t*     book,
                                          rv_workspace_t*      workspace,
                                          ra8_vsource_t*       sources,
                                          ra8_vsource_obj_t    objects[k_rv_max_objs],
                                          uint32_t*            object_id,
                                          rv_workspace_need_t* need)
{
  if (!priv_rv_workspace_require(budget, need)) {
    priv_rv_diag("reader_vmem: invalid workspace requirement\n");
    return false;
  }
  if (budget > (uint32_t)READER_VMEM_MAX_BUDGET ||
      need->total_bytes > sizeof(s_reader_workspace.bytes)) {
    internal_report_workspace(budget, need);
    return false;
  }
  if (!priv_rv_workspace_bind(s_reader_workspace.bytes,
                              sizeof(s_reader_workspace.bytes),
                              need,
                              workspace) ||
      ra8_vsource_init(sources, objects, (uint32_t)k_rv_max_objs) != k_ra8_ok ||
      ra8_vsource_add_paged(sources, internal_book_read, (void*)book, 0U, book->bytes, object_id) !=
        k_ra8_ok) {
    priv_rv_diag("reader_vmem: cache workspace/source setup failed\n");
    return false;
  }
  return true;
}

/**
 * @brief Execute one complete cache run and atomic trace transaction.
 * @details Composes workspace/source/cache state, drives every workload phase,
 * validates production statistics, and publishes only a complete synced trace.
 * @param[in] trace_path Final trace destination.
 * @param[in] budget Requested frame budget.
 * @param[in,out] driver Caller-owned workload state initialized with the seed.
 * @return Whether a complete trace was atomically published.
 * @retval true Run, statistics, sync, and publication succeeded.
 * @retval false Setup, cache, trace, or publication failed.
 * @pre @p trace_path is non-null and NUL-terminated; @p driver is writable.
 * @pre @p budget is non-zero and subject to the compiled maximum check.
 * @post Success publishes one complete deterministic trace and prints statistics.
 * @post Failure removes the owned private trace; pre-rename final is preserved.
 * @note Not thread-safe through production cache global/static limits.
 * @since 0.1.0
 */
RA8_INTERNAL static bool
internal_execute(const char* trace_path, uint32_t budget, rv_driver_t* driver)
{
  internal_layout_book(driver);
  const rv_book_t book = {
    .bytes = (uint64_t)driver->total_frames * (uint64_t)k_rv_frame_bytes,
  };
  rv_workspace_t      workspace              = {};
  rv_workspace_need_t need                   = {};
  ra8_vsource_obj_t   objects[k_rv_max_objs] = {};
  ra8_vsource_t       sources                = {};
  if (!internal_prepare(budget, &book, &workspace, &sources, objects, &driver->object_id, &need)) {
    return false;
  }
  ra8_vmem_t vm = {};
  if (!internal_vmem_setup(&vm, &workspace, budget, &sources)) {
    priv_rv_diag("reader_vmem: vmem setup failed\n");
    return false;
  }
  rv_trace_t trace = {};
  if (!priv_rv_trace_begin(trace_path, &trace)) {
    priv_rv_diag("reader_vmem: cannot open trace file ");
    priv_rv_diag(trace_path);
    priv_rv_diag("\n");
    return false;
  }
  driver->vm    = &vm;
  driver->trace = &trace;
  internal_phase_linear(driver);
  internal_phase_toc(driver);
  internal_phase_scan(driver);
  uint32_t   hits      = 0U;
  uint32_t   misses    = 0U;
  uint32_t   evictions = 0U;
  const bool stats_ok  = ra8_vmem_stats(&vm, &hits, &misses, &evictions) == k_ra8_ok;
  const bool run_ok    = !driver->failed && stats_ok;
  if (!run_ok || !priv_rv_trace_commit(&trace)) {
    priv_rv_trace_abort(&trace);
    priv_rv_diag("reader_vmem: trace generation failed\n");
    return false;
  }
  internal_report(driver, &book, budget, trace_path, hits, misses, evictions);
  return true;
}

/**
 * @brief Print CLI usage and the explicit compiled cache ceiling.
 * @details Uses bounded raw diagnostic fragments and integer formatting.
 * @pre Standard error may accept or reject output.
 * @pre ::READER_VMEM_MAX_BUDGET is representable by the decimal helper.
 * @post Usage and maximum-budget text were attempted.
 * @post No filesystem, cache, or workspace state changed.
 * @note Best-effort diagnostic failures are intentionally ignored.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_usage(void)
{
  priv_rv_diag("usage: reader_vmem [TRACE [FRAME_BUDGET]]\n");
  priv_rv_diag("compiled maximum frame budget: ");
  priv_rv_diag_u64(READER_VMEM_MAX_BUDGET);
  priv_rv_diag("\n");
}

int main(int argc, char** argv)
{
  if (argc > 3 || (argc > 1 && strcmp(argv[1], "--help") == 0)) {
    internal_usage();
    return (argc > 3) ? 2 : 0;
  }
  const char* trace_path = (argc > 1) ? argv[1] : "reader_vmem.trace";
  uint32_t    budget     = (uint32_t)k_rv_def_budget;
  if (argc > 2 && !internal_parse_budget(argv[2], &budget)) {
    internal_usage();
    return 2;
  }
  rv_driver_t driver = {.rng = (uint64_t)k_rv_rng_seed};
  return internal_execute(trace_path, budget, &driver) ? 0 : 1;
}
