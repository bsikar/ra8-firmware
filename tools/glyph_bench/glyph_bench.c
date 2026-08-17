/**
 * @file glyph_bench.c
 * @brief #147/#162/#164 glyph-cache workload sweep: drive the real
 *        ra8_glyph_atlas with a realistic text-render glyph stream and report the
 *        hit rate (and rasterisations saved) versus the cache budget.
 *
 * @details
 * HOST tool (not firmware). The Layer-3 glyph atlas earns its keep when the
 * renderer stops re-rasterising glyphs it just drew; this sweep quantifies that
 * on a realistic page-render workload so #164 can size the cell budget. It models
 * reading a book: each page draws a stream of glyphs from an English
 * letter-frequency distribution (plus spaces, capitals, punctuation, digits) at a
 * body font size with occasional headings, and page-turns re-render the same
 * working set. The stream is replayed through the ACTUAL ra8_glyph_atlas at a
 * range of budgets (cell counts); for each it reports the hit rate and the number
 * of render-on-miss calls (= rasterisations the renderer would actually perform).
 *
 * Cache hit/miss/eviction depends only on the key stream and the budget, not on
 * the bitmap content, so the render callback is a cheap deterministic stub -- the
 * measured hit rate is exact. Each cell in production is sized to the largest
 * glyph bitmap (2 * max font px squared, ~36 KiB), so the RAM cost of a budget is
 * `cells * ~36 KiB`; the table prints that so the knee is easy to read.
 *
 *
 * [Ring 7 / Tooling] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_glyph_atlas.h"
#include "ra8_io_stream.h"
#include "ra8_io_stream_posix.h"
#include "ra8_keycache.h"
#include "ra8_log.h"

/** @brief Workload model dimensions (no bare literals). */
typedef enum : uint32_t {
  k_gb_pages         = 200U,  /**< Pages rendered in the session.               */
  k_gb_page_glyphs   = 1800U, /**< Glyph draws per page (a dense page).         */
  k_gb_reread_pages  = 3U,    /**< Re-render each page this many times (turns). */
  k_gb_body_px       = 24U,   /**< Body text font size.                         */
  k_gb_heading_px    = 36U,   /**< Heading font size.                           */
  k_gb_heading_pct   = 3U,    /**< % of glyphs drawn at the heading size.       */
  k_gb_cap_pct       = 8U,    /**< % of letters drawn as capitals.              */
  k_gb_punct_pct     = 6U,    /**< % of glyphs drawn as punctuation.            */
  k_gb_digit_pct     = 2U,    /**< % of glyphs drawn as digits.                 */
  k_gb_pct_base      = 100U,  /**< Percentage base.                             */
  k_gb_cell_bytes    = 64U,   /**< Bench cell size (content-irrelevant).        */
  k_gb_buckets       = 512U,  /**< Hash buckets for the atlas.                  */
  k_gb_prod_cell_kib = 36U,   /**< Production cell cost (192x192 alpha8) KiB.   */
  k_gb_max_budget    = 256U,  /**< Largest swept atlas capacity.                */
  k_gb_decimal_scale = 100U,  /**< Two decimal digits for percentages.          */
  k_gb_dec_radix     = 10U,   /**< Decimal renderer radix.                      */
  k_gb_budget_width  = 5U,    /**< Budget report field width.                   */
  k_gb_ram_width     = 12U,   /**< RAM report field width.                      */
  k_gb_hit_width     = 7U,    /**< Whole-part width before `.NN`.               */
  k_gb_raster_width  = 14U,   /**< Rasterization report field width.            */
} gb_dim_t;

/** @brief xorshift64 + fill constants. */
typedef enum : uint32_t {
  k_gb_xs_a       = 13U, /**< xorshift shift 1. */
  k_gb_xs_b       = 7U,  /**< xorshift shift 2. */
  k_gb_xs_c       = 17U, /**< xorshift shift 3. */
  k_gb_ascii_az_a = 97U, /**< ASCII 'a'.        */
  k_gb_ascii_az_z = 26U, /**< Letters in a-z.   */
  k_gb_case_delta = 32U, /**< 'a' - 'A'.        */
  k_gb_ascii_0    = 48U, /**< ASCII '0'.        */
  k_gb_digits     = 10U, /**< Digit count.      */
} gb_const_t;

/** @brief Deterministic xorshift64 seed. */
typedef enum : uint64_t {
  k_gb_seed = 0xD1B54A32D192ED03ULL, /**< Fixed seed. */
} gb_seed_t;

/** @brief Swept cache budgets (cell counts). */
static const uint32_t s_gb_budgets[] =
  {16U, 32U, 48U, 64U, 96U, 128U, 192U, 256U}; /* MAGIC-OK: swept-budget data table */

/**
 * @brief English letter relative frequencies, scaled to a per-mille-ish table
 *        indexed by letter (a..z). Sums to ::k_gb_freq_total.
 */
/* MAGIC-OK: published English letter-frequency data table, not control constants. */
static const uint16_t s_gb_letter_freq[26] = {
  817U, 150U, 278U, 425U, 1270U, 223U, 202U,
  609U, 697U, 15U,  77U,  403U,  241U, /* MAGIC-OK: freq data */
  675U, 751U, 193U, 10U,  599U,  633U, 906U,
  276U, 98U,  236U, 15U,  197U,  7U}; /* MAGIC-OK: freq data */

/** @brief Sum of ::s_gb_letter_freq (the selection range). */
typedef enum : uint32_t {
  k_gb_freq_total = 9999U, /**< Sum of the 26 letter weights above. */
} gb_freq_t;

/** @brief Exact caller-owned backing for the largest measured atlas. */
typedef struct {
  uint8_t             cells[k_gb_max_budget * k_gb_cell_bytes]; /**< Bitmap cells.  */
  ra8_glyph_key_t     keys[k_gb_max_budget];                    /**< Cell keys.     */
  ra8_glyph_dims_t    dims[k_gb_max_budget];                    /**< Cell geometry. */
  ra8_keycache_cell_t meta[k_gb_max_budget];                    /**< LRU metadata.  */
  int32_t             buckets[k_gb_buckets];                    /**< Hash buckets.  */
} gb_workspace_t;

/** @brief Process-lifetime semantic backing for one measured atlas. */
static gb_workspace_t s_gb_workspace;
/** @brief Borrowed stdout/stderr portable stream handles. */
static ra8_io_stream_t s_gb_output;
static ra8_io_stream_t s_gb_diagnostic;
/** @brief Raw-descriptor adapter states for the borrowed process streams. */
static ra8_io_stream_posix_state_t s_gb_output_posix;
static ra8_io_stream_posix_state_t s_gb_diagnostic_posix;

/**
 * @brief Write a fixed sequence of NUL-terminated fragments exactly.
 * @details Iterates the borrowed fragments without formatting or staging so a
 * short/error result remains attributable to the destination stream.
 * @param[in,out] stream Bound destination stream.
 * @param[in] parts Fragment vector.
 * @param[in] count Number of entries in @p parts.
 * @return Canonical stream status.
 * @retval k_ra8_ok Every fragment was accepted.
 * @retval other The first rejected fragment's status.
 * @pre @p stream and @p parts are non-null.
 * @pre Every selected fragment is NUL-terminated.
 * @post Success writes fragments in array order without separators.
 * @post Failure stops before attempting later fragments.
 * @note Thread-safe across distinct streams.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_write_parts(ra8_io_stream_t* stream, const char* const* parts, size_t count)
{
  for (size_t i = 0U; i < count; ++i) {
    const ra8_err_t error = ra8_io_stream_puts(stream, parts[i]);
    if (error != k_ra8_ok) {
      return error;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Forward one logger byte into the bound diagnostic stream.
 * @details Adapts the repository logger's byte callback to the same bounded
 * stream contract used for normal benchmark diagnostics.
 * @param[in,out] context Bound ::ra8_io_stream_t destination.
 * @param[in] byte Byte emitted by the shared logger.
 * @return Nothing.
 * @pre @p context points to a bound stream.
 * @pre Logger calls are serialized by the benchmark thread.
 * @post The byte is offered exactly once to the diagnostic stream.
 * @post A rejected diagnostic byte is dropped without recursion.
 * @note The logger callback ABI cannot return a sink failure.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_log_byte(void* context, uint8_t byte)
{
  (void)ra8_io_stream_putc((ra8_io_stream_t*)context, (char)byte);
}

/**
 * @brief Bind process output descriptors and make broken pipes observable.
 * @details Installs non-owning raw-descriptor adapters for the process streams,
 * then binds the logger only after both adapters are valid.
 * @return Canonical stream-composition status.
 * @retval k_ra8_ok Both borrowed descriptors are bound.
 * @retval k_ra8_err_comm_error SIGPIPE disposition setup failed.
 * @retval other A raw-descriptor binding failed.
 * @pre No output has been attempted.
 * @pre Standard output and standard error remain process-owned.
 * @post Success binds both streams for the process lifetime.
 * @post SIGPIPE is ignored so the backend reports EPIPE.
 * @note Host composition only; descriptor ownership is not transferred.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_output_init(void)
{
  struct sigaction action = {.sa_handler = SIG_IGN};
  if ((sigemptyset(&action.sa_mask) != 0) || (sigaction(SIGPIPE, &action, nullptr) != 0)) {
    return k_ra8_err_comm_error;
  }
  ra8_err_t error = ra8_io_stream_posix_init(&s_gb_output, &s_gb_output_posix, STDOUT_FILENO);
  if (error == k_ra8_ok) {
    error = ra8_io_stream_posix_init(&s_gb_diagnostic, &s_gb_diagnostic_posix, STDERR_FILENO);
  }
  if (error == k_ra8_ok) {
    ra8_log_init();
    ra8_log_set_byte_sink(internal_log_byte, &s_gb_diagnostic);
  }
  return error;
}

/**
 * @brief Advance a fixed-seed xorshift64 generator and return the new state.
 *
 * @details
 * The standard xorshift64 recurrence (shifts 13/7/17) over @p s, updated in
 * place. Deterministic across runs and platforms, so a given seed always
 * produces the same glyph stream -- which is what makes the benched hit rate
 * exactly reproducible.
 *
 * @param[in,out] s The 64-bit generator state; replaced with the next state.
 *
 * @return The next 64-bit state value.
 * @retval 0 Never, for a non-zero seed: xorshift64 cannot reach 0 from a
 *           non-zero state, and ::k_gb_seed is non-zero.
 *
 * @pre @p s is non-NULL.
 * @pre @p s was seeded non-zero (::k_gb_seed).
 * @post @p s holds the advanced state.
 * @post The return value equals the new @p s.
 *
 * @note Not thread-safe; each caller owns its own state word.
 * @since 0.1.0
 */
RA8_INTERNAL static uint64_t internal_gb_rng(uint64_t* s)
{
  uint64_t x = *s;
  x ^= x << (uint32_t)k_gb_xs_a;
  x ^= x >> (uint32_t)k_gb_xs_b;
  x ^= x << (uint32_t)k_gb_xs_c;
  *s = x;
  return x;
}

/**
 * @brief Draw a uniform pseudo-random integer in [0, @p span).
 *
 * @details
 * Advances ::internal_gb_rng once and reduces modulo @p span. The modulo bias is
 * negligible for the small spans this workload uses and does not affect the
 * cache hit/miss decision being measured.
 *
 * @param[in,out] s    Generator state, advanced by one step when @p span > 0.
 * @param[in]     span Exclusive upper bound of the range.
 *
 * @return A value in [0, @p span).
 * @retval 0 @p span was 0 (an empty range is reported as 0).
 *
 * @pre @p s is non-NULL and seeded.
 * @pre @p span fits the intended selection range.
 * @post @p s is advanced by exactly one step when @p span is non-zero.
 * @post The result is strictly less than @p span (or 0 when @p span is 0).
 *
 * @note Not thread-safe; shares @p s with the caller.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_gb_below(uint64_t* s, uint32_t span)
{
  return (span == 0U) ? 0U : (uint32_t)(internal_gb_rng(s) % (uint64_t)span);
}

/**
 * @brief Pick a lowercase letter codepoint weighted by English frequency.
 *
 * @details
 * Draws a value in [0, ::k_gb_freq_total) and walks the cumulative
 * ::s_gb_letter_freq table, so 'e'/'t'/'a' appear far more often than 'q'/'z'
 * -- the distribution that makes the glyph cache behave like real text. The
 * final fallthrough returns 'a' only if a rounding gap leaves the draw
 * unmatched.
 *
 * @param[in,out] s Generator state, advanced during selection.
 *
 * @return An ASCII lowercase codepoint in 'a'..'z'.
 * @retval 97 ('a') the frequency walk fell through (draw exceeded the table sum).
 *
 * @pre @p s is non-NULL and seeded.
 * @pre ::s_gb_letter_freq sums to ::k_gb_freq_total.
 * @post @p s is advanced.
 * @post The result is a valid lowercase-letter codepoint.
 *
 * @note Not thread-safe; shares @p s with the caller.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_gb_pick_letter(uint64_t* s)
{
  uint32_t r = internal_gb_below(s, (uint32_t)k_gb_freq_total);
  for (uint32_t i = 0U; i < (uint32_t)k_gb_ascii_az_z; ++i) {
    if (r < (uint32_t)s_gb_letter_freq[i]) {
      return (uint32_t)k_gb_ascii_az_a + i;
    }
    r -= (uint32_t)s_gb_letter_freq[i];
  }
  return (uint32_t)k_gb_ascii_az_a; /* fallthrough: 'a' */
}

/** @brief A small punctuation/space repertoire (codepoints). */
static const uint32_t s_gb_punct[] = {' ', '.', ',', ';', ':', '\'', '"', '-', '!', '?'};

/**
 * @brief Choose one codepoint for the glyph stream from the weighted mix.
 *
 * @details
 * Rolls once to split the repertoire into punctuation/space
 * (::k_gb_punct_pct), digits (::k_gb_digit_pct) and letters (the remainder);
 * for a letter it draws a frequency-weighted lowercase codepoint
 * (::internal_gb_pick_letter) and upper-cases it with probability ::k_gb_cap_pct. The
 * distribution mirrors body text so the atlas working set matches a real page.
 *
 * @param[in,out] s Generator state, advanced during selection.
 *
 * @return The chosen glyph codepoint (letter, digit, or punctuation/space).
 * @retval 32 (space) when the punctuation roll selected the first repertoire
 *            entry, ::s_gb_punct[0].
 *
 * @pre @p s is non-NULL and seeded.
 * @pre The percentage constants sum to at most ::k_gb_pct_base.
 * @post @p s is advanced by one or more steps.
 * @post The result is a printable ASCII codepoint.
 *
 * @note Not thread-safe; shares @p s with the caller.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_gb_pick_codepoint(uint64_t* s)
{
  const uint32_t roll = internal_gb_below(s, (uint32_t)k_gb_pct_base);
  if (roll < (uint32_t)k_gb_punct_pct) {
    const uint32_t n = (uint32_t)(sizeof(s_gb_punct) / sizeof(s_gb_punct[0]));
    return s_gb_punct[internal_gb_below(s, n)];
  }
  if (roll < (uint32_t)k_gb_punct_pct + (uint32_t)k_gb_digit_pct) {
    return (uint32_t)k_gb_ascii_0 + internal_gb_below(s, (uint32_t)k_gb_digits);
  }
  uint32_t cp = internal_gb_pick_letter(s);
  if (internal_gb_below(s, (uint32_t)k_gb_pct_base) < (uint32_t)k_gb_cap_pct) {
    cp -= (uint32_t)k_gb_case_delta; /* uppercase variant */
  }
  return cp;
}

/**
 * @brief Atlas render-on-miss callback: stand in for one rasterisation.
 *
 * @details
 * The atlas invokes this on a cache miss to fill the freed cell. Hit / miss /
 * eviction depends only on the key stream and the budget, not on the bitmap
 * content, so this stub just zeroes the cell and reports a 1x1 glyph -- each
 * call therefore counts as exactly one rasterisation the renderer would have
 * performed, keeping the measured hit rate exact while the bench stays cheap.
 *
 * @param[in]  ctx        Unused render context (the bench carries no state).
 * @param[in]  key        Unused glyph key (content is irrelevant to hit rate).
 * @param[out] cell       Cell memory to fill; zeroed here.
 * @param[in]  cell_bytes Size of @p cell in bytes.
 * @param[out] out_w      Receives the glyph width (1).
 * @param[out] out_h      Receives the glyph height (1).
 *
 * @return Render status.
 * @retval k_ra8_ok Always; the stub cannot fail.
 *
 * @pre @p cell is non-NULL with at least @p cell_bytes bytes.
 * @pre @p out_w and @p out_h are non-NULL.
 * @post @p cell is zeroed and @p out_w / @p out_h are set to 1.
 * @post No global state is touched.
 *
 * @note Not thread-safe; matches the single-threaded bench driver.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_gb_render(void*                  ctx,
                                                 const ra8_glyph_key_t* key,
                                                 uint8_t*               cell,
                                                 uint32_t               cell_bytes,
                                                 uint16_t*              out_w,
                                                 uint16_t*              out_h)
{
  (void)ctx;
  (void)key;
  (void)memset(cell, 0, (size_t)cell_bytes);
  *out_w = 1U;
  *out_h = 1U;
  return k_ra8_ok;
}

/** @brief Total glyph accesses in the deterministic session. */
static const uint64_t s_gb_access_total =
  (uint64_t)k_gb_pages * (uint64_t)k_gb_reread_pages * (uint64_t)k_gb_page_glyphs;

/**
 * @brief Generate and submit one deterministic glyph access.
 * @details Advances the supplied page-local PRNG through the legacy
 * heading/codepoint choices, then balances every successful atlas get with a
 * matching put.
 * @param[in,out] atlas Initialized measured atlas.
 * @param[in,out] rng Page-local generator state.
 * @return Canonical atlas status.
 * @retval k_ra8_ok One glyph access completed.
 * @retval other The atlas rejected the get or matching put operation.
 * @pre @p atlas and @p rng are non-null.
 * @pre @p rng is a non-zero page-start or continuation state.
 * @post Success submits and releases exactly one glyph.
 * @post @p rng advances by the workload's fixed selection sequence.
 * @note Bitmap content is irrelevant; ::internal_gb_render fills each miss.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_submit_access(ra8_glyph_atlas_t* atlas, uint64_t* rng)
{
  const bool heading = internal_gb_below(rng, (uint32_t)k_gb_pct_base) < (uint32_t)k_gb_heading_pct;
  ra8_glyph_key_t key   = {.glyph_id = internal_gb_pick_codepoint(rng),
                           .size_px  = heading ? (uint16_t)k_gb_heading_px : (uint16_t)k_gb_body_px};
  ra8_glyph_t     glyph = {};
  ra8_err_t       error = ra8_glyph_atlas_get(atlas, &key, &glyph);
  if (error == k_ra8_ok) {
    error = ra8_glyph_atlas_put(atlas, glyph.bitmap);
  }
  return error;
}

/**
 * @brief Regenerate and replay the exact legacy session without a trace buffer.
 * @details Saves each page's PRNG start state, regenerates it for all rereads,
 *          then advances the session from the first pass's ending state.
 * @param[in,out] atlas Initialized measured atlas.
 * @return Canonical atlas status.
 * @retval k_ra8_ok The full deterministic workload completed.
 * @retval other The first glyph access failure.
 * @pre @p atlas is non-null and ready for ::ra8_glyph_atlas_get.
 * @pre The fixed seed is non-zero.
 * @post Success submits exactly ::s_gb_access_total accesses.
 * @post No generated trace bytes are retained.
 * @note Deterministic and thread-safe across distinct atlases.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_replay_workload(ra8_glyph_atlas_t* atlas)
{
  uint64_t session_rng = (uint64_t)k_gb_seed;
  for (uint32_t page = 0U; page < (uint32_t)k_gb_pages; ++page) {
    const uint64_t page_start = session_rng;
    uint64_t       next_page  = page_start;
    for (uint32_t repeat = 0U; repeat < (uint32_t)k_gb_reread_pages; ++repeat) {
      uint64_t page_rng = page_start;
      for (uint32_t glyph = 0U; glyph < (uint32_t)k_gb_page_glyphs; ++glyph) {
        const ra8_err_t error = internal_submit_access(atlas, &page_rng);
        if (error != k_ra8_ok) {
          return error;
        }
      }
      if (repeat == 0U) {
        next_page = page_rng;
      }
    }
    session_rng = next_page;
  }
  return k_ra8_ok;
}

/**
 * @brief Execute one budget against the caller-visible fixed atlas backing.
 * @details Clears and binds the exact semantic workspace, replays the complete
 * deterministic session, then publishes counters only after replay succeeds.
 * @param[in] budget Active atlas cell count.
 * @param[out] out_hits Measured cache hits.
 * @param[out] out_rasters Measured misses/rasterizations.
 * @return Canonical atlas or workload status.
 * @retval k_ra8_ok The budget completed and both counters were published.
 * @retval other Atlas initialization, replay, or statistics failed.
 * @pre @p budget is non-zero and at most ::k_gb_max_budget.
 * @pre Output pointers are non-null.
 * @post Success publishes the exact hit and miss counters.
 * @post The process workspace is reusable for the next budget.
 * @note Not thread-safe because it reuses ::s_gb_workspace.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_run_budget(uint32_t budget, uint32_t* out_hits, uint32_t* out_rasters)
{
  (void)memset(&s_gb_workspace, 0, sizeof(s_gb_workspace));
  const ra8_glyph_atlas_cfg_t cfg   = {.cell_mem     = s_gb_workspace.cells,
                                       .cell_bytes   = (uint32_t)k_gb_cell_bytes,
                                       .cell_count   = budget,
                                       .meta         = s_gb_workspace.meta,
                                       .keys         = s_gb_workspace.keys,
                                       .dims         = s_gb_workspace.dims,
                                       .buckets      = s_gb_workspace.buckets,
                                       .bucket_count = (uint32_t)k_gb_buckets,
                                       .render       = internal_gb_render,
                                       .render_ctx   = nullptr};
  ra8_glyph_atlas_t           atlas = {};
  ra8_err_t                   error = ra8_glyph_atlas_init(&atlas, &cfg);
  if (error == k_ra8_ok) {
    error = internal_replay_workload(&atlas);
  }
  if (error == k_ra8_ok) {
    error = ra8_glyph_atlas_stats(&atlas, out_hits, out_rasters, nullptr);
  }
  return error;
}

/**
 * @brief Write one unsigned value left-padded to a fixed field width.
 * @details Counts decimal digits arithmetically, emits the required leading
 * spaces, and delegates the value conversion to the bounded stream helper.
 * @param[in,out] stream Bound destination stream.
 * @param[in] value Value to render.
 * @param[in] width Minimum field width.
 * @return Canonical stream status.
 * @retval k_ra8_ok The complete padded value was accepted.
 * @retval other The first rejected padding or value write.
 * @pre @p stream is non-null and bound.
 * @pre @p width is a small human-report width.
 * @post Success writes spaces followed by the complete decimal value.
 * @post Failure stops at the first rejected write.
 * @note Thread-safe across distinct streams.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_put_padded_u64(ra8_io_stream_t* stream, uint64_t value, uint32_t width)
{
  uint32_t digits = 1U;
  for (uint64_t copy = value; copy >= (uint64_t)k_gb_dec_radix; copy /= (uint64_t)k_gb_dec_radix) {
    ++digits;
  }
  for (uint32_t i = digits; i < width; ++i) {
    const ra8_err_t error = ra8_io_stream_putc(stream, ' ');
    if (error != k_ra8_ok) {
      return error;
    }
  }
  return ra8_io_stream_put_u64(stream, value);
}

/**
 * @brief Write the fixed report preamble and workload dimensions.
 * @details Emits the legacy report as bounded constant and integer fragments,
 * avoiding locale, varargs formatting, and a large transient line buffer.
 * @return Canonical output-stream status.
 * @retval k_ra8_ok The complete preamble was accepted.
 * @retval other The first rejected fragment or integer write.
 * @pre ::internal_output_init completed successfully.
 * @pre The workload constants retain their documented values.
 * @post Success writes the exact legacy preamble.
 * @post Failure stops before attempting later fields.
 * @note Not thread-safe with concurrent writes to ::s_gb_output.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_report_header(void)
{
  const char* const prefix[] = {"# #147/#164 glyph-cache budget sweep\n\nWorkload: "};
  ra8_err_t         error    = internal_write_parts(&s_gb_output, prefix, 1U);
  if (error == k_ra8_ok) {
    error = ra8_io_stream_put_u32(&s_gb_output, (uint32_t)k_gb_pages);
  }
  if (error == k_ra8_ok) {
    error = ra8_io_stream_puts(&s_gb_output, " pages x ");
  }
  if (error == k_ra8_ok) {
    error = ra8_io_stream_put_u32(&s_gb_output, (uint32_t)k_gb_reread_pages);
  }
  if (error == k_ra8_ok) {
    error = ra8_io_stream_puts(&s_gb_output, " re-renders x ");
  }
  if (error == k_ra8_ok) {
    error = ra8_io_stream_put_u32(&s_gb_output, (uint32_t)k_gb_page_glyphs);
  }
  if (error == k_ra8_ok) {
    error = ra8_io_stream_puts(&s_gb_output, " glyphs = ");
  }
  if (error == k_ra8_ok) {
    error = ra8_io_stream_put_u64(&s_gb_output, s_gb_access_total);
  }
  if (error == k_ra8_ok) {
    error =
      ra8_io_stream_puts(&s_gb_output,
                         " glyph gets\n(English letter frequencies + caps/punct/digits; body ");
  }
  if (error == k_ra8_ok) {
    error = ra8_io_stream_put_u32(&s_gb_output, (uint32_t)k_gb_body_px);
  }
  if (error == k_ra8_ok) {
    error = ra8_io_stream_puts(&s_gb_output, "px, heading ");
  }
  if (error == k_ra8_ok) {
    error = ra8_io_stream_put_u32(&s_gb_output, (uint32_t)k_gb_heading_px);
  }
  if (error == k_ra8_ok) {
    error = ra8_io_stream_puts(
      &s_gb_output,
      "px)\n\n| cells | RAM @36KiB/cell | hit rate % | rasterisations |\n|------:|----------------:|-----------:|---------------:|\n");
  }
  return error;
}

/**
 * @brief Write one exact table row from integer hit/miss evidence.
 * @details Computes the rounded two-decimal percentage with checked-width
 * integer fields so report bytes do not depend on locale or floating point.
 * @param[in] budget Measured cell budget.
 * @param[in] hits Measured hits.
 * @param[in] rasters Measured misses/rasterizations.
 * @return Canonical output-stream status.
 * @retval k_ra8_ok The complete row was accepted.
 * @retval other The first rejected row fragment or integer write.
 * @pre Counters came from one complete ::internal_run_budget result.
 * @pre ::s_gb_access_total is non-zero.
 * @post Success writes one newline-terminated legacy-compatible row.
 * @post Percentage rounding is integer, locale-independent, and two-decimal.
 * @note Not thread-safe with concurrent writes to ::s_gb_output.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_report_row(uint32_t budget, uint32_t hits, uint32_t rasters)
{
  const uint64_t scaled =
    (((uint64_t)hits * (uint64_t)k_gb_pct_base * (uint64_t)k_gb_decimal_scale) +
     (s_gb_access_total / 2U)) /
    s_gb_access_total;
  const uint64_t whole    = scaled / (uint64_t)k_gb_decimal_scale;
  const uint32_t fraction = (uint32_t)(scaled % (uint64_t)k_gb_decimal_scale);
  ra8_err_t      error    = ra8_io_stream_puts(&s_gb_output, "| ");
  if (error == k_ra8_ok) {
    error = internal_put_padded_u64(&s_gb_output, budget, (uint32_t)k_gb_budget_width);
  }
  if (error == k_ra8_ok) {
    error = ra8_io_stream_puts(&s_gb_output, " | ");
  }
  if (error == k_ra8_ok) {
    error = internal_put_padded_u64(&s_gb_output,
                                    (uint64_t)budget * (uint64_t)k_gb_prod_cell_kib,
                                    (uint32_t)k_gb_ram_width);
  }
  if (error == k_ra8_ok) {
    error = ra8_io_stream_puts(&s_gb_output, " KiB | ");
  }
  if (error == k_ra8_ok) {
    error = internal_put_padded_u64(&s_gb_output, whole, (uint32_t)k_gb_hit_width);
  }
  if (error == k_ra8_ok) {
    error = ra8_io_stream_putc(&s_gb_output, '.');
  }
  if ((error == k_ra8_ok) && (fraction < (uint32_t)k_gb_dec_radix)) {
    error = ra8_io_stream_putc(&s_gb_output, '0');
  }
  if (error == k_ra8_ok) {
    error = ra8_io_stream_put_u32(&s_gb_output, fraction);
  }
  if (error == k_ra8_ok) {
    error = ra8_io_stream_puts(&s_gb_output, " | ");
  }
  if (error == k_ra8_ok) {
    error = internal_put_padded_u64(&s_gb_output, rasters, (uint32_t)k_gb_raster_width);
  }
  if (error == k_ra8_ok) {
    error = ra8_io_stream_puts(&s_gb_output, " |\n");
  }
  return error;
}

/**
 * @brief Run the complete deterministic cache-budget report.
 * @details Writes the preamble, measures each fixed budget in order using the
 * single semantic workspace, and appends the legacy no-cache comparison.
 * @return Canonical atlas or output status.
 * @retval k_ra8_ok Every measurement and report write completed.
 * @retval other The first atlas, workload, or stream failure.
 * @pre Process streams are bound and the fixed workspace is exclusively owned.
 * @pre Every swept budget is at most ::k_gb_max_budget.
 * @post Success writes the byte-exact legacy report.
 * @post No generated trace or atlas allocation survives the run.
 * @note Single-threaded composition root.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_run_report(void)
{
  ra8_err_t    error = internal_report_header();
  const size_t count = sizeof(s_gb_budgets) / sizeof(s_gb_budgets[0]);
  for (size_t i = 0U; (i < count) && (error == k_ra8_ok); ++i) {
    uint32_t hits    = 0U;
    uint32_t rasters = 0U;
    error            = internal_run_budget(s_gb_budgets[i], &hits, &rasters);
    if (error == k_ra8_ok) {
      error = internal_report_row(s_gb_budgets[i], hits, rasters);
    }
  }
  if (error == k_ra8_ok) {
    error = ra8_io_stream_puts(&s_gb_output,
                               "\nWithout a cache the renderer rasterises once per glyph get (");
  }
  if (error == k_ra8_ok) {
    error = ra8_io_stream_put_u64(&s_gb_output, s_gb_access_total);
  }
  if (error == k_ra8_ok) {
    error = ra8_io_stream_puts(&s_gb_output, ").\n");
  }
  return error;
}

int main(void)
{
  if (internal_output_init() != k_ra8_ok) {
    return 1;
  }
  const ra8_err_t error = internal_run_report();
  if (error != k_ra8_ok) {
    const char* const parts[] = {"glyph_bench: workload or output failed\n"};
    (void)internal_write_parts(&s_gb_diagnostic, parts, 1U);
    return 1;
  }
  return 0;
}
