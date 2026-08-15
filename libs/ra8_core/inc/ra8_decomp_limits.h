/**
 * @file ra8_decomp_limits.h
 * @brief Unified decompression-limits policy: one bound set for every decoder.
 * @ingroup grp_core
 *
 * @details
 * Every archive / compressed-stream decoder in this firmware (ZIP-store and
 * DEFLATE via miniz, RAR4/RAR5, gzip, XZ/LZMA2, tar, and the raw-DEFLATE
 * buffer path in `ra8_io_compress.h`) consumes untrusted SD-card content and
 * must be *bounded* and *fail-closed* on any hostile input. This header is
 * the single enforcement seam they all share: one policy record
 * (::ra8_decomp_limits_t), one running-budget tracker
 * (::ra8_decomp_budget_t), and one header-level declared-size check
 * (::ra8_decomp_check_declared).
 *
 * The threat model (owner-approved) is *not* remote code execution -- it is a
 * malicious or malformed archive crashing the reader or exhausting RAM/CPU.
 * The policy therefore bounds the five resource axes a decompressor can be
 * driven along:
 *
 * | Axis                | Field              | Breach error                    |
 * |---------------------|--------------------|---------------------------------|
 * | Total output bytes  | `max_output_bytes` | ::k_ra8_err_decomp_output_cap   |
 * | Compression ratio   | `max_ratio`        | ::k_ra8_err_decomp_ratio        |
 * | Archive entry count | `max_entries`      | ::k_ra8_err_decomp_entries      |
 * | Container nesting   | `max_depth`        | ::k_ra8_err_decomp_depth        |
 * | Decode-loop turns   | `max_iterations`   | ::k_ra8_err_decomp_iterations   |
 *
 * @par Semantics of each bound
 * - `max_output_bytes` applies per *decode unit*: one archive member or one
 *   wrapped stream (a `.tar.gz`'s whole decompressed tar is one unit). It is
 *   NOT a lifetime sum, so re-reading pages of a long comic never exhausts it.
 * - `max_ratio` is enforced as `output <= (input * max_ratio) +
 *   ratio_grace_bytes`. The additive grace keeps tiny legitimate inputs
 *   (a 40-byte gzip of a 4 KiB file) from tripping a pure quotient, while a
 *   real decompression bomb blows through both terms immediately.
 * - `max_entries` applies per archive enumeration (central directory walk,
 *   RAR block walk, tar header walk).
 * - `max_depth` counts stacked decode layers (stream-in-stream). Readers
 *   never recurse into member archives by construction, so depth only
 *   guards the wrapper plumbing (e.g. gzip -> tar is depth 2).
 * - `max_iterations` is a NASA P10 Rule 2 backstop on every decode loop
 *   whose trip count depends on untrusted bytes: each pass charges one
 *   iteration, so a stream engineered to make no progress terminates.
 *
 * Header-declared sizes (a ZIP central-directory record, a RAR block header,
 * a tar size field) are checked against the same policy *before* any
 * decoding starts (::ra8_decomp_check_declared) -- a lying header is
 * rejected as cheaply as an honest bomb.
 *
 * @note All checks fail closed: the first breach returns a specific
 *       `k_ra8_err_decomp_*` code and the decoder must stop. Reject and
 *       continue; never crash.
 *
 * @see ra8_unarch_gzip.h  gzip member decoder built on this policy.
 * @see ra8_unarch_tar.h   tar walker built on this policy.
 * @see ra8_unarch_xz.h    XZ/LZMA2 decoder built on this policy.
 * @see ra8_comic.h        Comic facade whose CBZ/CBR/CBT backends charge it.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since Version 0.1.0
 *
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @enum ra8_decomp_defaults_t
 * @brief Owner-approved default values for ::ra8_decomp_limits_t.
 * @details One 64-bit-typed enum so every default is a named constant (no
 *          magic numbers) in a single place. `ra8_decomp_limits_default()`
 *          returns a policy built from exactly these values. Rationale per
 *          value:
 *          - output cap 64 MiB: the SDRAM working-set ceiling -- no single
 *            decode unit may exceed the board's big memory.
 *          - ratio 1024:1 (+64 KiB grace): above DEFLATE's theoretical
 *            ~1032:1 per-layer maximum lies only bomb territory; the grace
 *            admits small honest files whose fixed headers skew the ratio.
 *          - 4096 entries: an order of magnitude above the largest real
 *            comic volume / EPUB spine.
 *          - depth 2: wrapper stream + inner container (`.tar.gz`); nothing
 *            legitimate stacks deeper.
 *          - 1 Mi iterations: decode loops charge one turn per bounded work
 *            chunk (>= one input or output block), far below this for any
 *            in-cap stream.
 * @since Version 0.1.0
 */
typedef enum : uint64_t {
  k_ra8_decomp_def_output_bytes = 64ULL * 1024ULL * 1024ULL, /**< Per-unit output cap (64 MiB).  */
  k_ra8_decomp_def_max_ratio    = 1024U,                     /**< Output:input ratio bound.      */
  k_ra8_decomp_def_ratio_grace  = 64U * 1024U,               /**< Additive ratio grace (64 KiB). */
  k_ra8_decomp_def_max_entries  = 4096U,                     /**< Per-archive entry cap.         */
  k_ra8_decomp_def_max_iters    = 1048576U,                  /**< Loop iteration budget (1 Mi).  */
  k_ra8_decomp_def_max_depth    = 2U,                        /**< Stacked decode-layer cap.      */
} ra8_decomp_defaults_t;

/**
 * @struct ra8_decomp_limits_t
 * @brief One decompression policy: the five resource bounds decoders enforce.
 *
 * @details Value type -- copy freely. Build one with
 *          ::ra8_decomp_limits_default and tighten fields as needed (tests
 *          use small limits to exercise breach paths cheaply). Every field
 *          must be non-zero: a zero bound would mean "reject everything"
 *          for caps and "unlimited" ambiguity for the grace, so
 *          ::ra8_decomp_budget_init validates all six.
 *
 * @invariant All fields are non-zero once validated by
 *            ::ra8_decomp_budget_init.
 *
 * @par Example:
 * @code
 * ra8_decomp_limits_t lim = ra8_decomp_limits_default();
 * lim.max_output_bytes    = 1024U * 1024U; // tighten for a host test
 * @endcode
 *
 * @see ra8_decomp_budget_t
 * @since Version 0.1.0
 */
typedef struct {
  uint64_t max_output_bytes;  /**< Per-decode-unit output cap, bytes.          */
  uint32_t max_ratio;         /**< Output:input ratio bound (multiplier).      */
  uint32_t ratio_grace_bytes; /**< Additive output grace before ratio applies. */
  uint32_t max_entries;       /**< Per-archive enumerated-entry cap.           */
  uint32_t max_iterations;    /**< Per-decode-loop iteration budget.           */
  uint8_t  max_depth;         /**< Stacked decode-layer (nesting) cap.         */
} ra8_decomp_limits_t;

/**
 * @struct ra8_decomp_budget_t
 * @brief Running consumption tracker charged by a decoder against its policy.
 *
 * @details One budget tracks one decode unit (streaming decoders) or one
 *          archive enumeration (walkers). Decoders charge it as they go:
 *          ::ra8_decomp_budget_charge_output after emitting bytes,
 *          ::ra8_decomp_budget_charge_entry per enumerated member,
 *          ::ra8_decomp_budget_charge_iter per decode-loop turn, and
 *          ::ra8_decomp_budget_enter / ::ra8_decomp_budget_leave around a
 *          stacked layer. Each charge fails closed with the axis-specific
 *          error the moment its bound is crossed.
 *
 * @invariant `depth <= limits.max_depth` between charges.
 * @invariant Counters only grow (except `depth`, which is balanced by
 *            enter/leave).
 *
 * @par Example:
 * @code
 * ra8_decomp_budget_t b = {};
 * (void)ra8_decomp_budget_init(&b, nullptr); // default policy
 * while (more_input) {
 *   RA8_RETURN_ON_ERROR(ra8_decomp_budget_charge_iter(&b), tag, "loop cap");
 *   // ... decode one chunk producing n bytes from in_total consumed ...
 *   RA8_RETURN_ON_ERROR(ra8_decomp_budget_charge_output(&b, in_total, n), tag, "bomb");
 * }
 * @endcode
 *
 * @see ra8_decomp_limits_t
 * @since Version 0.1.0
 */
typedef struct {
  ra8_decomp_limits_t limits;    /**< Policy in force for this budget.    */
  uint64_t            out_bytes; /**< Decompressed bytes produced so far. */
  uint64_t            in_bytes;  /**< Compressed bytes consumed so far.   */
  uint32_t            entries;   /**< Archive entries enumerated so far.  */
  uint32_t            iters;     /**< Decode-loop turns charged so far.   */
  uint8_t             depth;     /**< Current stacked decode-layer depth. */
} ra8_decomp_budget_t;

/**
 * @brief The owner-approved default decompression policy.
 *
 * @details Builds a ::ra8_decomp_limits_t from the
 *          ::ra8_decomp_defaults_t constants. This is the ONE policy every
 *          production decoder runs under; tests tighten copies of it to
 *          exercise breach paths with small fixtures.
 *
 * @return The default policy by value (all fields non-zero).
 * @retval ra8_decomp_limits_t Populated from ::ra8_decomp_defaults_t.
 *
 * @pre None -- pure constant constructor.
 * @pre No initialization is required before calling.
 * @post Every field of the result is non-zero.
 * @post The result validates under ::ra8_decomp_budget_init.
 *
 * @note Thread-safe: returns a value, touches no state.
 * @see ra8_decomp_budget_init()
 * @since Version 0.1.0
 */
[[nodiscard]] ra8_decomp_limits_t ra8_decomp_limits_default(void);

/**
 * @brief Bind a budget to a policy (or the default policy) and zero it.
 *
 * @details Copies @p limits (or the default when NULL) into @p b and clears
 *          every counter. Rejects a policy with any zero field: a zero cap
 *          is always a configuration bug, never a meaningful bound.
 *
 * @param[out] b      Budget to initialise (non-NULL).
 * @param[in]  limits Policy to enforce, or NULL for the default policy.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Budget zeroed and bound to a valid policy.
 * @retval k_ra8_err_null_ptr    @p b was NULL.
 * @retval k_ra8_err_invalid_arg @p limits has a zero field.
 *
 * @pre @p b addresses a writable ::ra8_decomp_budget_t.
 * @pre @p limits, when non-NULL, has every field non-zero.
 * @post On k_ra8_ok all counters are zero and `b->limits` is the policy.
 * @post On any error @p b is left zeroed (unusable until re-initialised).
 *
 * @note Not thread-safe with respect to the same budget.
 * @see ra8_decomp_limits_default()
 * @since Version 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_decomp_budget_init(ra8_decomp_budget_t*       b,
                                               const ra8_decomp_limits_t* limits);

/**
 * @brief Charge decompressed output against the cap and ratio bounds.
 *
 * @details Adds @p out_delta to the produced-bytes counter, records the
 *          input consumed so far, then enforces (1) the per-unit output cap
 *          and (2) the bomb bound `out <= in * max_ratio + grace`. Call it
 *          after every emitted chunk so a bomb is caught within one chunk
 *          of the bound, long before RAM or CPU is exhausted.
 *
 * @param[in,out] b         Initialised budget (non-NULL).
 * @param[in]     in_total  Compressed bytes consumed so far (monotonic).
 * @param[in]     out_delta Newly produced decompressed bytes (may be 0).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                    Charge recorded; both bounds hold.
 * @retval k_ra8_err_null_ptr          @p b was NULL.
 * @retval k_ra8_err_decomp_output_cap Output now exceeds `max_output_bytes`.
 * @retval k_ra8_err_decomp_ratio     Output now exceeds the ratio bound.
 *
 * @pre @p b was initialised by ::ra8_decomp_budget_init.
 * @pre @p in_total is non-decreasing across calls on the same budget.
 * @post On k_ra8_ok, `b->out_bytes` includes @p out_delta.
 * @post On any breach the counters still reflect the attempted charge (the
 *       decoder must stop; the budget is not reusable after a breach).
 *
 * @note Not thread-safe with respect to the same budget.
 * @see ra8_decomp_check_declared()
 * @since Version 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_decomp_budget_charge_output(ra8_decomp_budget_t* b, uint64_t in_total, uint64_t out_delta);

/**
 * @brief Charge one enumerated archive entry against the entry cap.
 *
 * @details Walkers call this once per member they index (ZIP central
 *          directory record, RAR file block, tar header). The
 *          many-tiny-entries bomb is rejected the moment the count crosses
 *          `max_entries`.
 *
 * @param[in,out] b Initialised budget (non-NULL).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                 Entry recorded within the cap.
 * @retval k_ra8_err_null_ptr       @p b was NULL.
 * @retval k_ra8_err_decomp_entries The entry count now exceeds `max_entries`.
 *
 * @pre @p b was initialised by ::ra8_decomp_budget_init.
 * @pre The caller charges exactly once per enumerated member.
 * @post On k_ra8_ok, `b->entries` grew by one.
 * @post On breach the walker must stop and reject the archive.
 *
 * @note Not thread-safe with respect to the same budget.
 * @see ra8_decomp_budget_charge_iter()
 * @since Version 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_decomp_budget_charge_entry(ra8_decomp_budget_t* b);

/**
 * @brief Charge one decode-loop turn against the iteration budget.
 *
 * @details The NASA P10 Rule 2 backstop for loops whose trip count depends
 *          on untrusted bytes: each pass through such a loop charges one
 *          turn, so a stream engineered to stall (no input consumed, no
 *          output produced) still terminates within `max_iterations`.
 *
 * @param[in,out] b Initialised budget (non-NULL).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                    Turn recorded within the budget.
 * @retval k_ra8_err_null_ptr          @p b was NULL.
 * @retval k_ra8_err_decomp_iterations The turn count now exceeds
 *                                     `max_iterations`.
 *
 * @pre @p b was initialised by ::ra8_decomp_budget_init.
 * @pre The charging loop performs bounded work per turn.
 * @post On k_ra8_ok, `b->iters` grew by one.
 * @post On breach the decode loop must stop fail-closed.
 *
 * @note Not thread-safe with respect to the same budget.
 * @see ra8_decomp_budget_charge_entry()
 * @since Version 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_decomp_budget_charge_iter(ra8_decomp_budget_t* b);

/**
 * @brief Enter one stacked decode layer (nesting-depth guard).
 *
 * @details Charge before constructing an inner decoder over an outer one
 *          (e.g. the tar walker over a gzip stream view). The
 *          archive-in-archive bomb is rejected before the inner layer does
 *          any work.
 *
 * @param[in,out] b Initialised budget (non-NULL).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Depth recorded within `max_depth`.
 * @retval k_ra8_err_null_ptr     @p b was NULL.
 * @retval k_ra8_err_decomp_depth The depth now exceeds `max_depth`.
 *
 * @pre @p b was initialised by ::ra8_decomp_budget_init.
 * @pre Every successful enter is balanced by ::ra8_decomp_budget_leave.
 * @post On k_ra8_ok, `b->depth` grew by one.
 * @post On breach no inner decode may start.
 *
 * @note Not thread-safe with respect to the same budget.
 * @see ra8_decomp_budget_leave()
 * @since Version 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_decomp_budget_enter(ra8_decomp_budget_t* b);

/**
 * @brief Leave one stacked decode layer (balances ::ra8_decomp_budget_enter).
 *
 * @details Decrements the depth counter; a NULL budget or an already-zero
 *          depth is ignored (teardown paths call this unconditionally).
 *
 * @param[in,out] b Budget to unwind (may be NULL).
 *
 * @pre @p b, when non-NULL, was initialised by ::ra8_decomp_budget_init.
 * @pre The call balances a successful ::ra8_decomp_budget_enter.
 * @post A non-zero depth decreased by one.
 * @post A NULL @p b or zero depth left everything unchanged.
 *
 * @note Not thread-safe with respect to the same budget.
 * @see ra8_decomp_budget_enter()
 * @since Version 0.1.0
 */
void ra8_decomp_budget_leave(ra8_decomp_budget_t* b);

/**
 * @brief Header-level check of a member's declared sizes against a policy.
 *
 * @details Rejects a lying or hostile header before any decoding: the
 *          declared decompressed size must fit the per-unit output cap and
 *          the ratio bound relative to the declared compressed size. Used
 *          by every walker at index time (ZIP central directory, RAR block
 *          header, tar size field) so hostile members cost O(1).
 *
 * @param[in] limits    Policy to check against (non-NULL, validated fields).
 * @param[in] comp_size Declared compressed (packed) size in bytes.
 * @param[in] out_size  Declared decompressed (unpacked) size in bytes.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                    Both declared sizes are within policy.
 * @retval k_ra8_err_null_ptr          @p limits was NULL.
 * @retval k_ra8_err_decomp_output_cap @p out_size exceeds `max_output_bytes`.
 * @retval k_ra8_err_decomp_ratio     @p out_size exceeds the ratio bound.
 *
 * @pre @p limits has every field non-zero (policy from
 *      ::ra8_decomp_limits_default or validated by
 *      ::ra8_decomp_budget_init).
 * @pre Sizes are the raw header-declared values (untrusted).
 * @post No state is modified (pure check).
 * @post A k_ra8_ok result guarantees nothing about the *actual* stream --
 *       running enforcement still applies during decode.
 *
 * @note Thread-safe: pure read.
 * @see ra8_decomp_budget_charge_output()
 * @since Version 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_decomp_check_declared(const ra8_decomp_limits_t* limits, uint64_t comp_size, uint64_t out_size);

/**
 * @typedef ra8_decomp_read_fn
 * @brief Positioned reader used by bounded container preflights.
 * @details Reads from an immutable seekable container without requiring a resident blob.
 * @param[in] ctx Opaque backing context.
 * @param[in] offset Absolute byte offset in the container.
 * @param[out] buf Destination for @p len bytes.
 * @param[in] len Requested byte count.
 * @return Bytes actually read; a short result leaves validation to the format decoder.
 * @note The callback must not retain @p buf.
 * @since Version 0.1.0
 */
typedef size_t (*ra8_decomp_read_fn)(void* ctx, uint64_t offset, void* buf, size_t len);

/**
 * @brief Reject an over-cap ZIP from its EOCD before directory allocation.
 * @details Scans only the bounded classic-ZIP comment window in fixed chunks.
 *          A valid EOCD whose total-entry field exceeds the shared policy is
 *          rejected before a bounded format allocator can obscure the cause
 *          with a generic allocation or validation failure.
 * @param[in] read Seekable container callback.
 * @param[in] ctx Opaque callback context.
 * @param[in] archive_size Exact ZIP byte length.
 * @return Preflight status.
 * @retval k_ra8_ok No valid over-cap EOCD was found; the ZIP decoder remains authoritative.
 * @retval k_ra8_err_null_ptr @p read was NULL.
 * @retval k_ra8_err_decomp_entries The EOCD declares more than 4096 entries.
 * @pre @p archive_size is the same size later supplied to the ZIP decoder.
 * @pre A successful callback returns exactly the requested byte count.
 * @post No archive byte or callback context is modified by this function.
 * @post Read failures do not replace the ZIP decoder's final validation result.
 * @note ZIP64's saturated 16-bit entry count is necessarily above this policy.
 * @since Version 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_decomp_zip_entry_preflight(ra8_decomp_read_fn read, void* ctx, uint64_t archive_size);

#ifdef __cplusplus
}
#endif
