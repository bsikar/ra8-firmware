/**
 * @file emu_prof.c
 * @brief Firmware profiler implementation (see emu_prof.h)
 *
 * @details
 * The RA8_EMU_PROFILE profiler: FUNC-symbol collection, the wall-time
 * sampler, the exact per-instruction hook with call-chain reconstruction,
 * and the run-end reports (speedscope JSON export, the self-contained HTML
 * flamechart, the boot timeline, and the inclusive/self table) -- moved
 * verbatim out of the ra8_emulator main translation unit.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "emu_prof.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "emu_elf.h"
#include "emu_host_io_internal.h"

/** @brief Nanoseconds per second (timespec tv_nsec -> seconds). */
static const double s_nsec_per_sec = 1.0e9;

/** @brief Monotonic wall-clock seconds. */
double board_now_s(void)
{
  struct timespec ts = {};
  (void)clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + ((double)ts.tv_nsec / s_nsec_per_sec);
}

/* ===========================================================================
 * Firmware profiler (RA8_EMU_PROFILE). Two modes, bucketed by ELF FUNC symbol:
 *   =1     wall-time sample -- charge each chunk's wall time to its start PC.
 *          Cheap (no per-instruction cost); a flat % list of the dominant cost.
 *   =full  per-instruction -- a code hook tallies every instruction + call entry
 *          AND reconstructs the live call chain (see below), so the run end emits
 *          an Ozone-style breakdown: a boot timeline, an inclusive/self table,
 *          and a speedscope flamechart file. Accurate but ~10x slower; off by
 *          default. The run auto-stops once boot settles into the idle frame
 *          loop, so =full profiles boot work rather than the idle tail.
 * ===========================================================================
 */
enum : uint32_t {
  k_prof_max_syms = 8192U, /**< Cap on profiled FUNC symbols.      */
  k_prof_top_n    = 40U,   /**< Top entries printed in the report. */
};
typedef struct {
  uint32_t lo;          /**< Function entry (Thumb bit cleared). */
  uint32_t hi;          /**< Function end (lo + st_size).        */
  uint64_t name_offset; /**< Source offset of the symbol name.   */
  double   secs;        /**< Wall seconds (wall mode).           */
  uint64_t insns;       /**< Instructions executed (insn mode).  */
  uint64_t calls;       /**< Entries to this fn (insn mode).     */
} prof_sym_t;
static prof_sym_t              s_prof[k_prof_max_syms];
static const emu_elf_source_t* s_prof_elf; /**< Borrowed run-long source for names. */
static uint32_t                s_prof_n       = 0U;
static double                  s_prof_total_s = 0.0;
static uint64_t                s_prof_total_i = 0U;
static prof_mode_t             s_prof_mode    = k_prof_off;

/* ---------------------------------------------------------------------------
 * Ozone-style call-stack tracing (insn mode only). On top of the per-function
 * tally above, reconstruct the live call chain straight from the PC stream: a
 * fresh function entry that is not already on the chain is a call (push), and
 * re-entering a function already deeper on the chain is a return (pop down to
 * it). NASA Rule 1 bans recursion in this firmware, so a function appears at
 * most once on the chain and the "already on the chain" test is unambiguous.
 * The chain is sampled at a fixed instruction cadence into a bounded,
 * chronological store and written out as a speedscope "sampled" profile -- open
 * ra8_emulator_profile.speedscope.json at https://speedscope.app for the
 * time-ordered flamechart ("what ran when", the Ozone timeline) plus the
 * sandwich view (self vs total per function). WFI idle naturally weighs ~zero
 * because a halted core retires no instructions, so the picture is boot work,
 * not the idle frame loop. ===============================================
 */
enum : uint32_t {
  k_prof_max_depth   = 64U,    /**< Deepest call chain captured per sample.  */
  k_prof_max_samples = 16384U, /**< Chronological stack samples (decimated). */
  k_prof_samp_every  = 256U,   /**< Default instructions per chain sample.   */
};
static uint16_t s_pstk[k_prof_max_depth];                      /**< Live chain.                */
static uint32_t s_pstk_n = 0U;                                 /**< Chain depth.               */
static uint16_t s_samp[k_prof_max_samples][k_prof_max_depth];  /**< root..leaf.                */
static uint8_t  s_samp_d[k_prof_max_samples];                  /**< Per-sample chain depth.    */
static uint32_t s_samp_w[k_prof_max_samples];                  /**< Per-sample weight (insns). */
static uint32_t s_samp_n        = 0U;                          /**< Stored sample count.       */
static uint64_t s_samp_every    = (uint64_t)k_prof_samp_every; /**< Insns per sample (>>x2).   */
static uint64_t s_samp_acc      = 0U;                          /**< Insns since last sample.   */
static uint32_t s_prof_stop_pc  = 0U;                          /**< RA8_EMU_STOP_PC (0=off).   */
static bool     s_prof_stop_hit = false;                       /**< Set when STOP_PC reached.  */
static uint64_t s_incl[k_prof_max_syms];                       /**< Inclusive weight (report). */
static uint64_t s_self[k_prof_max_syms];                       /**< Self (leaf) weight.        */

/**
 * @brief qsort comparator: order the FUNC symbols by entry address.
 * @details Qsort comparator: order the func symbols by entry address; this step is contained within the emu prof model and uses bounded caller or module-owned storage.
 * @param[in] a A input used by the operation.
 * @param[in] b B input used by the operation.
 * @return The prof cmp result produced by the emu prof model.
 * @retval value The operation-specific prof cmp value.
 * @pre Arguments satisfy the ranges documented for prof cmp. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu prof model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static int internal_prof_cmp(const void* a, const void* b)
{
  const uint32_t la = ((const prof_sym_t*)a)->lo;
  const uint32_t lb = ((const prof_sym_t*)b)->lo;
  if (la < lb) {
    return -1;
  }
  if (la > lb) {
    return 1;
  }
  return 0;
}

/**
 * @brief Collect one sized STT_FUNC symbol into the bounded profiler table.
 * @details Retains only address bounds and the immutable source-name offset.
 * @param[in] symbol Bounds-checked symbol entry.
 * @param[in] ctx Unused callback context.
 * @return Whether symbol iteration should continue.
 * @retval true The symbol was skipped or collected below the fixed cap.
 * @retval false The fixed profiler-symbol capacity was exhausted.
 * @pre @p symbol is non-null.
 * @pre The profiler source remains open.
 * @post `s_prof_n` never exceeds ::k_prof_max_syms.
 * @post Collected entries retain no pointer into source bytes.
 * @note Not thread-safe; profiler setup is single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_prof_symbol(const emu_elf_symbol_t* symbol, void* ctx)
{
  (void)ctx;
  if (((symbol->info & (uint8_t)k_elf_st_type_mask) != 2U) || (symbol->size == 0U) ||
      (symbol->name_offset == 0U)) {
    return true;
  }
  if (s_prof_n >= (uint32_t)k_prof_max_syms) {
    return false;
  }
  const uint32_t lo = symbol->value & ~1U;
  s_prof[s_prof_n] =
    (prof_sym_t){.lo = lo, .hi = lo + symbol->size, .name_offset = symbol->name_offset};
  s_prof_n++;
  return true;
}

/** @brief Collect + sort FUNC symbols (RA8_EMU_PROFILE only) for PC bucketing. */
void prof_load(const emu_elf_source_t* elf)
{
  const char* mode = getenv("RA8_EMU_PROFILE");
  if (mode == nullptr) {
    s_prof_mode = k_prof_off;
    return;
  }
  s_prof_mode =
    ((strcmp(mode, "full") == 0) || (strcmp(mode, "insn") == 0)) ? k_prof_insn : k_prof_wall;
  s_prof_n   = 0U;
  s_prof_elf = elf;
  (void)elf_foreach_symbol(elf, internal_prof_symbol, nullptr);
  qsort(s_prof, (size_t)s_prof_n, sizeof(s_prof[0]), internal_prof_cmp);
  (void)priv_emu_io_errf("  [profile] %s; %u FUNC symbols\n",
                         (s_prof_mode == k_prof_insn) ? "per-instruction (exact, slow)"
                                                      : "wall-time sample",
                         (unsigned)s_prof_n);
}

/**
 * @brief Binary-search the FUNC symbol owning @p pc; returns s_prof_n if none.
 * @details Binary-search the func symbol owning @p pc; returns s_prof_n if none; this step is contained within the emu prof model and uses bounded caller or module-owned storage.
 * @param[in] pc Guest program-counter value associated with the operation.
 * @return The prof find result produced by the emu prof model.
 * @retval value The operation-specific prof find value.
 * @pre Arguments satisfy the ranges documented for prof find. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu prof model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_prof_find(uint32_t pc)
{
  uint32_t lo = 0U;
  uint32_t hi = s_prof_n;
  while (lo < hi) {
    const uint32_t mid = lo + ((hi - lo) / 2U);
    if (s_prof[mid].lo <= pc) {
      lo = mid + 1U;
    } else {
      hi = mid;
    }
  }
  if (lo == 0U) {
    return s_prof_n;
  }
  const uint32_t idx = lo - 1U;
  return (pc < s_prof[idx].hi) ? idx : s_prof_n;
}

/** @brief Attribute @p dt wall seconds to @p pc's function (wall-sample mode). */
void prof_add(uint32_t pc, double dt)
{
  if (s_prof_mode != k_prof_wall) {
    return;
  }
  s_prof_total_s += dt;
  const uint32_t idx = internal_prof_find(pc);
  if (idx < s_prof_n) {
    s_prof[idx].secs += dt;
  }
}

/**
 * @brief Halve the sample store (merge adjacent pairs) when it fills up.
 * @details Halve the sample store (merge adjacent pairs) when it fills up; this step is contained within the emu prof model and uses bounded caller or module-owned storage.
 * @pre Arguments satisfy the ranges documented for prof decimate. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu prof model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_prof_decimate(void)
{
  uint32_t dst = 0U;
  for (uint32_t i = 0U; i < s_samp_n; i += 2U) {
    const uint32_t w2 = ((i + 1U) < s_samp_n) ? s_samp_w[i + 1U] : 0U;
    if (dst != i) {
      (void)memcpy(s_samp[dst], s_samp[i], (size_t)s_samp_d[i] * sizeof(uint16_t));
      s_samp_d[dst] = s_samp_d[i];
    }
    s_samp_w[dst] = s_samp_w[i] + w2; /* merged time keeps the total exact. */
    dst++;
  }
  s_samp_n = dst;
  s_samp_every *= 2U; /* coarser cadence keeps the next fill the same span. */
}

/**
 * @brief Append the live call chain as one chronological sample of @p weight insns.
 * @details Append the live call chain as one chronological sample of @p weight insns; this step is contained within the emu prof model and uses bounded caller or module-owned storage.
 * @param[in] weight Weight input used by the operation.
 * @pre Arguments satisfy the ranges documented for prof sample. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu prof model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_prof_sample(uint32_t weight)
{
  if (s_samp_n >= (uint32_t)k_prof_max_samples) {
    internal_prof_decimate();
  }
  uint32_t d = s_pstk_n;
  if (d > (uint32_t)k_prof_max_depth) {
    d = (uint32_t)k_prof_max_depth;
  }
  for (uint32_t i = 0U; i < d; i++) {
    s_samp[s_samp_n][i] = s_pstk[i];
  }
  s_samp_d[s_samp_n] = (uint8_t)d;
  s_samp_w[s_samp_n] = weight;
  s_samp_n++;
}

/**
 * @brief Fold @p f (PC's owning FUNC index) into the live call chain (push/pop).
 * @details Fold @p f (pc's owning func index) into the live call chain (push/pop); this step is contained within the emu prof model and uses bounded caller or module-owned storage.
 * @param[in] f F input used by the operation.
 * @pre Arguments satisfy the ranges documented for prof stack update. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu prof model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_prof_stack_update(uint32_t f)
{
  if (f >= s_prof_n) {
    return; /* unknown region -- keep the current leaf (it gets the self time). */
  }
  if ((s_pstk_n > 0U) && (s_pstk[s_pstk_n - 1U] == (uint16_t)f)) {
    return; /* still in the same function -- no call/return transition. */
  }
  for (uint32_t i = s_pstk_n; i > 0U; i--) {
    if (s_pstk[i - 1U] == (uint16_t)f) {
      s_pstk_n = i; /* returned to a frame already on the chain -- unwind to it. */
      return;
    }
  }
  if (s_pstk_n < (uint32_t)k_prof_max_depth) {
    s_pstk[s_pstk_n] = (uint16_t)f; /* a fresh call -- push it. */
    s_pstk_n++;
  }
}

/* cppcheck-suppress constParameterCallback ; UC_HOOK_CODE callback ABI is void*. */
/**
 * @brief Perform prof insn hook for the emu prof model.
 * @details Perform prof insn hook for the emu prof model; this step is contained within the emu prof model and uses bounded caller or module-owned storage.
 * @param[in,out] uc Unicorn engine whose emulated state is read or updated.
 * @param[in] address Guest address involved in the operation.
 * @param[in] size Size of the requested region or access in bytes.
 * @param[in,out] user Hook context supplied when the callback was registered.
 * @pre Arguments satisfy the ranges documented for prof insn hook. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu prof model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_prof_insn_hook(uc_engine* uc, uint64_t address, uint32_t size, void* user)
{
  (void)size;
  (void)user;
  s_prof_total_i++;
  const uint32_t idx = internal_prof_find((uint32_t)address);
  if (idx < s_prof_n) {
    s_prof[idx].insns++;
    if ((uint32_t)address == s_prof[idx].lo) {
      s_prof[idx].calls++; /* PC at the entry point -> a fresh call (approx). */
    }
  }
  internal_prof_stack_update(idx);
  s_samp_acc++;
  if (s_samp_acc >= s_samp_every) {
    internal_prof_sample((uint32_t)s_samp_acc);
    s_samp_acc = 0U;
  }
  if ((s_prof_stop_pc != 0U) && ((uint32_t)address == s_prof_stop_pc)) {
    s_prof_stop_hit = true; /* RA8_EMU_STOP_PC reached -- end the run cleanly. */
    (void)uc_emu_stop(uc);
  }
}

/**
 * @brief Convert a host-I/O result to the profiler writer's boolean accumulator.
 * @details Keeps each writer expression explicit while preserving the first failure.
 * @param[in] result Raw descriptor operation result.
 * @return True only for complete success.
 * @retval true @p result reports k_emu_io_ok.
 * @retval false @p result reports any failure or truncation.
 * @pre @p result is fully initialised.
 * @pre The caller has not discarded an earlier failure.
 * @post No profiler or descriptor state changes.
 * @post The returned value depends only on @p result.status.
 * @note Pure value predicate; thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_prof_io_ok(emu_io_result_t result)
{
  return result.status == k_emu_io_ok;
}

/** @brief Sink configuration for one streamed profiler symbol name. */
typedef struct {
  int  fd;         /**< Explicit file descriptor, ignored for error sink. */
  char escape;     /**< Character escaped with backslash, or NUL.         */
  bool error_sink; /**< Route bytes to the injected error sink.           */
  bool ok;         /**< Sticky exact-output success.                      */
} prof_name_sink_t;

/**
 * @brief Emit one transient source-name chunk to a selected sink.
 * @details Writes directly to the chosen descriptor or injected error sink.
 * @param[in] bytes Name bytes without a terminating NUL.
 * @param[in] length Transient chunk length.
 * @param[in,out] opaque ::prof_name_sink_t output state.
 * @return True while every write succeeds.
 * @retval true The complete chunk was emitted.
 * @retval false A sink operation failed.
 * @pre @p bytes spans @p length readable bytes.
 * @pre @p opaque selects a valid sink.
 * @post Every input byte is emitted, with selected escaping.
 * @post Failure latches `ok=false` and stops string streaming.
 * @note Consumes the transient chunk before returning.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_prof_name_chunk(const char* bytes, size_t length, void* opaque)
{
  prof_name_sink_t* const sink = (prof_name_sink_t*)opaque;
  if ((sink->escape == '\0') && sink->error_sink) {
    sink->ok = internal_prof_io_ok(priv_emu_io_err_bytes(bytes, length));
    return sink->ok;
  }
  for (size_t index = 0U; (index < length) && sink->ok; index++) {
    if ((sink->escape != '\0') && (bytes[index] == sink->escape || bytes[index] == '\\')) {
      sink->ok = internal_prof_io_ok(priv_emu_io_file_char(sink->fd, '\\'));
    }
    if (sink->ok) {
      sink->ok = internal_prof_io_ok(priv_emu_io_file_char(sink->fd, bytes[index]));
    }
  }
  return sink->ok;
}

/**
 * @brief Stream one retained-offset symbol name to a file or error sink.
 * @details Resolves the retained offset against the still-open source in bounded chunks.
 * @param[in] symbol Profiler symbol index.
 * @param[in] fd Explicit file descriptor, ignored for error sink.
 * @param[in] escape File-sink quote character to escape, or NUL.
 * @param[in] error_sink Whether to use the injected error sink.
 * @return True only after a terminating NUL and exact output.
 * @retval true The complete name was emitted exactly.
 * @retval false The index/source was invalid or a read/write failed.
 * @pre @p symbol is below `s_prof_n` and `s_prof_elf` remains open.
 * @pre File mode supplies a valid @p fd.
 * @post Output bytes match the old resident-string walk exactly.
 * @post No source byte pointer survives the call.
 * @note Reads only bounded transient name chunks.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_prof_name(uint32_t symbol, int fd, char escape, bool error_sink)
{
  if ((symbol >= s_prof_n) || (s_prof_elf == nullptr)) {
    return false;
  }
  prof_name_sink_t sink = {.fd = fd, .escape = escape, .error_sink = error_sink, .ok = true};
  return elf_string_foreach(s_prof_elf,
                            s_prof[symbol].name_offset,
                            internal_prof_name_chunk,
                            &sink) &&
         sink.ok;
}

/**
 * @brief Write the speedscope `shared.frames` array: one entry per symbol.
 * @details Escapes JSON quote and backslash characters while copying ELF symbol names.
 * @param[in] fd Raw output descriptor positioned after the `"frames":[`.
 * @return True only when every raw write completed.
 * @retval true All frame objects were written.
 * @retval false At least one descriptor operation failed.
 * @pre @p fd is a valid writable descriptor.
 * @pre `s_prof_n` frames are populated.
 * @post Exactly `s_prof_n` objects are attempted in order.
 * @post The enclosing bracket is left for the caller to close.
 * @note Not thread-safe; the profiler is single-threaded host-side.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_prof_json_frames(int fd)
{
  bool ok = true;
  for (uint32_t i = 0U; i < s_prof_n; i++) {
    ok = internal_prof_io_ok(
           priv_emu_io_file_text(fd, (i == 0U) ? "{\"name\":\"" : ",{\"name\":\"")) &&
         ok;
    ok = internal_prof_name(i, fd, '"', false) && ok;
    ok = internal_prof_io_ok(priv_emu_io_file_text(fd, "\"}")) && ok;
  }
  return ok;
}

/**
 * @brief Write the speedscope `samples` array: one frame-index stack per sample.
 * @details Emits each root-to-leaf frame-index vector with JSON delimiters.
 *
 * @param[in] fd Raw output descriptor positioned after the `"samples":[`.
 * @return True only when every raw write completed.
 * @retval true All sample arrays were written.
 * @retval false At least one descriptor operation failed.
 *
 * @pre @p fd is a valid writable descriptor.
 * @pre `s_samp_n` samples are populated with matching depths in `s_samp_d`.
 * @post Exactly `s_samp_n` comma-separated arrays are written.
 * @post The enclosing bracket is left for the caller to close.
 *
 * @note Not thread-safe; the profiler is single-threaded host-side.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_prof_json_samples(int fd)
{
  bool ok = true;
  for (uint32_t i = 0U; i < s_samp_n; i++) {
    ok = internal_prof_io_ok(priv_emu_io_file_char(fd, (i == 0U) ? '[' : ',')) && ok;
    if (i != 0U) {
      ok = internal_prof_io_ok(priv_emu_io_file_char(fd, '[')) && ok;
    }
    for (uint8_t j = 0U; j < s_samp_d[i]; j++) {
      ok = internal_prof_io_ok(
             priv_emu_io_filef(fd, (j == 0U) ? "%u" : ",%u", (unsigned)s_samp[i][j])) &&
           ok;
    }
    ok = internal_prof_io_ok(priv_emu_io_file_char(fd, ']')) && ok;
  }
  return ok;
}

/**
 * @brief Write the speedscope `weights` array, parallel to the samples array.
 * @details Emits the instruction weight corresponding to every captured sample.
 *
 * @param[in] fd Raw output descriptor positioned after the `"weights":[`.
 * @return True only when every raw write completed.
 * @retval true All weights were written.
 * @retval false At least one descriptor operation failed.
 *
 * @pre @p fd is a valid writable descriptor.
 * @pre `s_samp_w` holds `s_samp_n` weights.
 * @post Exactly `s_samp_n` comma-separated integers are written.
 * @post The enclosing bracket is left for the caller to close.
 *
 * @note Not thread-safe; the profiler is single-threaded host-side.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_prof_json_weights(int fd)
{
  bool ok = true;
  for (uint32_t i = 0U; i < s_samp_n; i++) {
    ok =
      internal_prof_io_ok(priv_emu_io_filef(fd, (i == 0U) ? "%u" : ",%u", (unsigned)s_samp_w[i])) &&
      ok;
  }
  return ok;
}

/**
 * @brief Write the flamechart page's `FRAMES` array as JavaScript string literals.
 *
 * @details
 * The same frame names as @ref internal_prof_json_frames, but the viewer markup quotes
 * with `'`, so `'` and `\` are the characters escaped here.
 *
 * @param[in] fd Raw output descriptor positioned after the `FRAMES=[`.
 * @return True only when every raw write completed.
 * @retval true All quoted names were written.
 * @retval false At least one descriptor operation failed.
 *
 * @pre @p fd is a valid writable descriptor.
 * @pre `s_prof_n` frames are populated.
 * @post Exactly `s_prof_n` comma-separated quoted names are written.
 * @post The enclosing bracket is left for the caller to close.
 *
 * @note Not thread-safe; the profiler is single-threaded host-side.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_prof_js_frames(int fd)
{
  bool ok = true;
  for (uint32_t i = 0U; i < s_prof_n; i++) {
    ok = internal_prof_io_ok(priv_emu_io_file_text(fd, (i == 0U) ? "'" : ",'")) && ok;
    ok = internal_prof_name(i, fd, '\'', false) && ok;
    ok = internal_prof_io_ok(priv_emu_io_file_char(fd, '\'')) && ok;
  }
  return ok;
}

/**
 * @brief Perform prof write speedscope for the emu prof model.
 * @details Perform prof write speedscope for the emu prof model; this step is contained within the emu prof model and uses bounded caller or module-owned storage.
 * @param[in] path NUL-terminated host path used by the operation.
 * @pre Arguments satisfy the ranges documented for prof write speedscope. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu prof model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_prof_write_speedscope(const char* path)
{
  if ((s_samp_n == 0U) || (s_prof_n == 0U)) {
    return;
  }
  emu_io_txn_t txn = {.fd = -1};
  if (priv_emu_io_txn_begin(path, &txn).status != k_emu_io_ok) {
    return;
  }
  uint64_t total = 0U;
  for (uint32_t i = 0U; i < s_samp_n; i++) {
    total += s_samp_w[i];
  }
  bool ok = internal_prof_io_ok(
    priv_emu_io_file_text(txn.fd,
                          "{\"$schema\":\"https://www.speedscope.app/file-format-schema.json\",\n"
                          " \"name\":\"ra8_emulator boot\",\"activeProfileIndex\":0,\n"
                          " \"shared\":{\"frames\":["));
  ok = internal_prof_json_frames(txn.fd) && ok;
  ok = internal_prof_io_ok(priv_emu_io_filef(
         txn.fd,
         "]},\n"
         " \"profiles\":[{\"type\":\"sampled\",\"name\":\"boot\",\"unit\":\"none\",\n"
         "  \"startValue\":0,\"endValue\":%llu,\n"
         "  \"samples\":[",
         (unsigned long long)total)) &&
       ok;
  ok = internal_prof_json_samples(txn.fd) && ok;
  ok = internal_prof_io_ok(priv_emu_io_file_text(txn.fd, "],\n  \"weights\":[")) && ok;
  ok = internal_prof_json_weights(txn.fd) && ok;
  ok = internal_prof_io_ok(priv_emu_io_file_text(txn.fd, "]}]}\n")) && ok;
  if (!ok || (priv_emu_io_txn_commit(&txn).status != k_emu_io_ok)) {
    priv_emu_io_txn_abort(&txn);
  }
}

/* Self-contained flamechart viewer markup. The page embeds the profile arrays
 * (written just before this) and renders a time-ordered flame chart on a canvas
 * -- the Ozone timeline, but as a local file that opens in any browser with no
 * upload and no external site. Single-quoted HTML/JS strings keep the C literal
 * free of escapes; pure 7-bit ASCII. */
static const char s_k_prof_html_head[] =
  "<!doctype html><html><head><meta charset='utf-8'><title>ra8_emulator profile</title>\n"
  "<style>\n"
  "body{margin:0;font:12px Menlo,monospace;background:#1e1e1e;color:#ddd}\n"
  "#bar{padding:7px 10px;background:#2a2a2a;border-bottom:1px solid #444}\n"
  "#bar b{color:#fff}#bar button,#bar input{font:11px monospace;margin-left:10px;"
  "background:#3a3a3a;color:#ddd;border:1px solid #555;padding:2px 6px}\n"
  "#tip{position:fixed;pointer-events:none;background:#000;color:#fff;padding:5px 7px;"
  "border:1px solid #888;display:none;white-space:nowrap;z-index:9;font:11px monospace}\n"
  "canvas{display:block;cursor:crosshair}\n"
  "</style></head><body>\n"
  "<div id='bar'><b id='title'></b><span id='info'></span>"
  "<button onclick='resetView()'>Reset zoom</button>"
  "search:<input id='q' size='18' oninput='onSearch()'></div>\n"
  "<canvas id='fc'></canvas><div id='tip'></div>\n<script>\n";

static const char s_k_prof_html_js[] =
  "var cv=document.getElementById('fc'),ctx=cv.getContext('2d'),tip=document.getElementById('tip');\n"
  "document.getElementById('title').textContent=TITLE;\n"
  "var ROW=18,total=0,i;for(i=0;i<WEIGHTS.length;i++)total+=WEIGHTS[i];\n"
  "var maxd=0;for(i=0;i<SAMPLES.length;i++)if(SAMPLES[i].length>maxd)maxd=SAMPLES[i].length;\n"
  "var rects=[],incl={},self={};\n"
  "for(var d=0;d<maxd;d++){var cum=0,rs=0,rf=-1,op=false;\n"
  " for(i=0;i<SAMPLES.length;i++){var ff=d<SAMPLES[i].length?SAMPLES[i][d]:-1;\n"
  "  if(!(op&&ff===rf)){if(op&&rf>=0)rects.push({d:d,a:rs,b:cum,f:rf});rs=cum;rf=ff;op=true;}\n"
  "  cum+=WEIGHTS[i];}\n"
  " if(op&&rf>=0)rects.push({d:d,a:rs,b:cum,f:rf});}\n"
  "for(i=0;i<SAMPLES.length;i++){var s=SAMPLES[i],w=WEIGHTS[i];\n"
  " for(var j=0;j<s.length;j++)incl[s[j]]=(incl[s[j]]||0)+w;\n"
  " if(s.length)self[s[s.length-1]]=(self[s[s.length-1]]||0)+w;}\n"
  "var vx0=0,vx1=total,q='';\n"
  "function col(fi){var n=FRAMES[fi],h=0,k;for(k=0;k<n.length;k++)h=(h*31+n.charCodeAt(k))&0xffffff;\n"
  " var lit=(q&&n.toLowerCase().indexOf(q)>=0);return 'hsl('+(h%359)+','+(lit?'90%':'48%')+','+(lit?'62%':'44%')+')';}\n"
  "function resize(){cv.width=window.innerWidth;cv.height=Math.max(maxd*ROW+4,160);draw();}\n"
  "function draw(){ctx.clearRect(0,0,cv.width,cv.height);var span=vx1-vx0;if(span<=0)return;\n"
  " ctx.font='11px monospace';ctx.textBaseline='middle';\n"
  " for(var r=0;r<rects.length;r++){var R=rects[r];if(R.b<=vx0||R.a>=vx1)continue;\n"
  "  var p0=(R.a-vx0)/span*cv.width,p1=(R.b-vx0)/span*cv.width,w=p1-p0;if(w<0.4)continue;\n"
  "  var y=R.d*ROW;ctx.fillStyle=col(R.f);ctx.fillRect(p0,y,Math.max(w-0.6,0.5),ROW-1);\n"
  "  if(w>34){ctx.fillStyle='#111';ctx.fillText(FRAMES[R.f],p0+3,y+ROW/2,w-6);}}}\n"
  "function pick(mx,my){var d=Math.floor(my/ROW),span=vx1-vx0,wx=vx0+mx/cv.width*span,r;\n"
  " for(r=0;r<rects.length;r++){var R=rects[r];if(R.d===d&&wx>=R.a&&wx<R.b)return R;}return null;}\n"
  "cv.onmousemove=function(e){var R=pick(e.offsetX,e.offsetY);if(!R){tip.style.display='none';return;}\n"
  " var n=FRAMES[R.f],to=incl[R.f]||0,se=self[R.f]||0;\n"
  " tip.innerHTML=n+'<br>this block: '+((R.b-R.a)/total*100).toFixed(2)+'% ('+(R.b-R.a)+' insns)'+\n"
  "  '<br>total '+(to/total*100).toFixed(2)+'%  self '+(se/total*100).toFixed(2)+'%';\n"
  " tip.style.display='block';tip.style.left=(e.clientX+14)+'priv_px';tip.style.top=(e.clientY+14)+'priv_px';};\n"
  "cv.onmouseleave=function(){tip.style.display='none';};\n"
  "cv.onclick=function(e){var R=pick(e.offsetX,e.offsetY);if(R){vx0=R.a;vx1=R.b;draw();}};\n"
  "function resetView(){vx0=0;vx1=total;draw();}\n"
  "function onSearch(){q=document.getElementById('q').value.toLowerCase();draw();}\n"
  "document.getElementById('info').textContent=' | '+SAMPLES.length+' samples, '+total+\n"
  "  ' insns  (hover for self/total, click a block to zoom, Reset to zoom out)';\n"
  "window.onresize=resize;resize();\n";

/**
 * @brief Write a self-contained, locally-openable HTML flamechart of the samples.
 * @details Write a self-contained, locally-openable html flamechart of the samples; this step is contained within the emu prof model and uses bounded caller or module-owned storage.
 * @param[in] path NUL-terminated host path used by the operation.
 * @param[in] total Total count used to normalize or report the operation.
 * @pre Arguments satisfy the ranges documented for prof write html. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu prof model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_prof_write_html(const char* path, uint64_t total)
{
  if ((s_samp_n == 0U) || (s_prof_n == 0U)) {
    return;
  }
  emu_io_txn_t txn = {.fd = -1};
  if (priv_emu_io_txn_begin(path, &txn).status != k_emu_io_ok) {
    return;
  }
  bool ok = internal_prof_io_ok(priv_emu_io_file_text(txn.fd, s_k_prof_html_head));
  ok      = internal_prof_io_ok(priv_emu_io_file_text(txn.fd, "var FRAMES=[")) && ok;
  ok      = internal_prof_js_frames(txn.fd) && ok;
  ok      = internal_prof_io_ok(priv_emu_io_file_text(txn.fd, "];\n")) && ok;
  /* SAMPLES / WEIGHTS are plain integer arrays, so the JSON the speedscope
   * writer emits is already valid JavaScript -- the same two helpers serve. */
  ok = internal_prof_io_ok(priv_emu_io_file_text(txn.fd, "var SAMPLES=[")) && ok;
  ok = internal_prof_json_samples(txn.fd) && ok;
  ok = internal_prof_io_ok(priv_emu_io_file_text(txn.fd, "];\nvar WEIGHTS=[")) && ok;
  ok = internal_prof_json_weights(txn.fd) && ok;
  ok = internal_prof_io_ok(
         priv_emu_io_filef(txn.fd,
                           "];\nvar TITLE='ra8_emulator flamechart -- %llu insns, %u samples';\n",
                           (unsigned long long)total,
                           (unsigned)s_samp_n)) &&
       ok;
  ok = internal_prof_io_ok(priv_emu_io_file_text(txn.fd, s_k_prof_html_js)) && ok;
  ok = internal_prof_io_ok(priv_emu_io_file_text(txn.fd, "</script></body></html>\n")) && ok;
  if (!ok || (priv_emu_io_txn_commit(&txn).status != k_emu_io_ok)) {
    priv_emu_io_txn_abort(&txn);
  }
}

/** @brief Fraction-to-per-cent scale (fraction * 100.0 == per-cent). */
static const double s_percent_scale = 100.0;

/** @brief Boot-timeline "phase" collapse constants for internal_prof_print_boot_timeline. */
typedef enum : uint32_t {
  k_no_fn        = 0xFFFFFFFFU, /**< Sentinel for "no phase frame".        */
  k_phase_depth  = 2U,          /**< Chain depth used as the boot "phase". */
  k_phase_lines  = 24U,         /**< Cap on printed timeline segments.     */
  k_phase_pct_x1 = 100U,        /**< Per-cent base: keep segments >= 1%.   */
} prof_phase_t;

/**
 * @brief Reset then fill s_incl/s_self from the samples; return total weight.
 * @details Reset then fill s_incl/s_self from the samples; return total weight; this step is contained within the emu prof model and uses bounded caller or module-owned storage.
 * @return The prof accumulate incl self result produced by the emu prof model.
 * @retval value The operation-specific prof accumulate incl self value.
 * @pre Arguments satisfy the ranges documented for prof accumulate incl self. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu prof model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static uint64_t internal_prof_accumulate_incl_self(void)
{
  /* Inclusive (anywhere on the chain) + self (leaf) weights, from the samples.
   * No recursion (NASA Rule 1) -> each function appears at most once per sample,
   * so a straight per-frame add needs no dedup. */
  uint64_t total = 0U;
  for (uint32_t i = 0U; i < s_prof_n; i++) {
    s_incl[i] = 0U;
    s_self[i] = 0U;
  }
  for (uint32_t i = 0U; i < s_samp_n; i++) {
    const uint32_t w = s_samp_w[i];
    const uint8_t  d = s_samp_d[i];
    total += w;
    for (uint8_t j = 0U; j < d; j++) {
      s_incl[s_samp[i][j]] += w;
    }
    if (d > 0U) {
      s_self[s_samp[i][d - 1U]] += w;
    }
  }
  return total;
}

/**
 * @brief Print the boot timeline: each contiguous shallow-depth phase run.
 * @details Print the boot timeline: each contiguous shallow-depth phase run; this step is contained within the emu prof model and uses bounded caller or module-owned storage.
 * @param[in] total Total count used to normalize or report the operation.
 * @pre Arguments satisfy the ranges documented for prof print boot timeline. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu prof model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_prof_print_boot_timeline(uint64_t total)
{
  /* Phase timeline: collapse each sample's chain to a fixed shallow depth (the
   * major subsystem under main) and print each contiguous run as a boot phase,
   * so the terminal shows "what ran when" even without opening speedscope. */
  (void)priv_emu_io_errf("  [profile] boot timeline (phase = call depth %u; start%% .. width%%):\n",
                         (unsigned)k_phase_depth);
  uint64_t cum   = 0U;
  uint64_t segw  = 0U;
  uint32_t segfn = (uint32_t)k_no_fn;
  uint32_t lines = 0U;
  for (uint32_t i = 0U; i <= s_samp_n; i++) {
    uint32_t fn = (uint32_t)k_no_fn;
    if (i < s_samp_n) {
      const uint8_t d = s_samp_d[i];
      if (d > 0U) {
        const uint8_t pd = ((uint32_t)(d - 1U) < (uint32_t)k_phase_depth) ? (uint8_t)(d - 1U)
                                                                          : (uint8_t)k_phase_depth;
        fn               = s_samp[i][pd];
      }
    }
    if ((i == s_samp_n) || (fn != segfn)) {
      const bool show = (segfn != (uint32_t)k_no_fn) &&
                        (((uint32_t)k_phase_pct_x1 * segw) >= total) &&
                        (lines < (uint32_t)k_phase_lines);
      if (show) {
        (void)priv_emu_io_errf("    %5.1f%%  +%4.1f%%  ",
                               s_percent_scale * (double)cum / (double)total,
                               s_percent_scale * (double)segw / (double)total);
        if (segfn < s_prof_n) {
          (void)internal_prof_name(segfn, -1, '\0', true);
        } else {
          (void)priv_emu_io_err_text("?");
        }
        (void)priv_emu_io_err_text("\n");
        lines++;
      }
      cum += segw;
      segw  = 0U;
      segfn = fn;
    }
    if (i < s_samp_n) {
      segw += s_samp_w[i];
    }
  }
}

/**
 * @brief Print the inclusive/self table (sorted by inclusive weight).
 * @details Print the inclusive/self table (sorted by inclusive weight); this step is contained within the emu prof model and uses bounded caller or module-owned storage.
 * @param[in] html Html input used by the operation.
 * @param[in] out Destination storage receiving the computed result.
 * @param[in] total Total count used to normalize or report the operation.
 * @pre Arguments satisfy the ranges documented for prof print incl self table. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu prof model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_prof_print_incl_self_table(const char* html, const char* out, uint64_t total)
{
  /* Inclusive/self table -- the "why is it slow" view (sorted by inclusive). */
  (void)priv_emu_io_errf("  [profile] flamechart GUI -> %s  (interactive: hover/zoom/search)\n"
                         "  [profile] %u samples over %llu insns  (also %s for speedscope.app)\n"
                         "       self%%    total%%  function\n",
                         html,
                         (unsigned)s_samp_n,
                         (unsigned long long)total,
                         out);
  for (uint32_t k = 0U; k < (uint32_t)k_prof_top_n; k++) {
    uint32_t best  = s_prof_n;
    uint64_t bestv = 0U;
    for (uint32_t i = 0U; i < s_prof_n; i++) {
      if (s_incl[i] > bestv) {
        bestv = s_incl[i];
        best  = i;
      }
    }
    if ((best == s_prof_n) || (bestv == 0U)) {
      break;
    }
    (void)priv_emu_io_errf("    %8.2f%%  %7.2f%%  ",
                           s_percent_scale * (double)s_self[best] / (double)total,
                           s_percent_scale * (double)s_incl[best] / (double)total);
    (void)internal_prof_name(best, -1, '\0', true);
    (void)priv_emu_io_err_text("\n");
    s_incl[best] = 0U; /* consume so the next pick is the runner-up. */
  }
}

/**
 * @brief Speedscope export + inclusive/self breakdown + phase timeline (insn mode).
 * @details Speedscope export + inclusive/self breakdown + phase timeline (insn mode); this step is contained within the emu prof model and uses bounded caller or module-owned storage.
 * @pre Arguments satisfy the ranges documented for prof report flamechart. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu prof model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_prof_report_flamechart(void)
{
  const char* out = getenv("RA8_EMU_PROFILE_OUT");
  if ((out == nullptr) || (out[0] == '\0')) {
    out = "ra8_emulator_profile.speedscope.json";
  }
  const char* html = getenv("RA8_EMU_PROFILE_HTML");
  if ((html == nullptr) || (html[0] == '\0')) {
    html = "ra8_emulator_profile.html";
  }
  internal_prof_write_speedscope(out);

  const uint64_t total = internal_prof_accumulate_incl_self();
  if (total == 0U) {
    return;
  }
  internal_prof_write_html(html, total); /* self-contained local GUI flamechart. */
  internal_prof_print_boot_timeline(total);
  internal_prof_print_incl_self_table(html, out, total);
}

/** @brief Print the top hot functions (by wall time or instruction count) at run end. */
void prof_report(void)
{
  const bool   insn = (s_prof_mode == k_prof_insn);
  const double tot  = insn ? (double)s_prof_total_i : s_prof_total_s;
  if ((s_prof_mode == k_prof_off) || (tot <= 0.0)) {
    return;
  }
  if (insn) {
    (void)priv_emu_io_errf("  [profile] %llu instructions; hottest (by instruction count):\n"
                           "     %%insn       instructions       calls  function\n",
                           (unsigned long long)s_prof_total_i);
  } else {
    (void)priv_emu_io_errf("  [profile] %.2fs wall sampled; hottest (by wall time):\n", tot);
  }
  for (uint32_t k = 0U; k < (uint32_t)k_prof_top_n; k++) {
    uint32_t best  = s_prof_n;
    double   bestv = 0.0;
    for (uint32_t i = 0U; i < s_prof_n; i++) {
      const double v = insn ? (double)s_prof[i].insns : s_prof[i].secs;
      if (v > bestv) {
        bestv = v;
        best  = i;
      }
    }
    if ((best == s_prof_n) || (bestv <= 0.0)) {
      break;
    }
    if (insn) {
      (void)priv_emu_io_errf("    %6.2f%%  %15llu  %10llu  ",
                             s_percent_scale * bestv / tot,
                             (unsigned long long)s_prof[best].insns,
                             (unsigned long long)s_prof[best].calls);
      (void)internal_prof_name(best, -1, '\0', true);
      (void)priv_emu_io_err_text("\n");
      s_prof[best].insns = 0U;
    } else {
      (void)priv_emu_io_errf("    %6.2f%%  %8.2fs  ", s_percent_scale * bestv / tot, bestv);
      (void)internal_prof_name(best, -1, '\0', true);
      (void)priv_emu_io_err_text("\n");
      s_prof[best].secs = 0.0;
    }
  }
  if (insn && (s_samp_n > 0U)) {
    internal_prof_report_flamechart(); /* speedscope export + inclusive/self + timeline. */
  }
}

/** @brief Implementation of `emu_prof_install()` -- arms the insn hook in insn mode. */
void emu_prof_install(uc_engine* uc)
{
  if (s_prof_mode == k_prof_insn) {
    static uc_hook local_h_prof;
    (void)
      uc_hook_add(uc, &local_h_prof, UC_HOOK_CODE, (void*)internal_prof_insn_hook, nullptr, 1, 0);
  }
}

/** @brief Implementation of `emu_prof_mode()` -- plain state read. */
prof_mode_t emu_prof_mode(void)
{
  return s_prof_mode;
}

/** @brief Implementation of `emu_prof_total_insns()` -- plain counter read. */
uint64_t emu_prof_total_insns(void)
{
  return s_prof_total_i;
}

/** @brief Implementation of `emu_prof_set_stop_pc()` -- plain state write. */
void emu_prof_set_stop_pc(uint32_t pc)
{
  s_prof_stop_pc = pc;
}

/** @brief Implementation of `emu_prof_stop_hit()` -- plain flag read. */
bool emu_prof_stop_hit(void)
{
  return s_prof_stop_hit;
}
